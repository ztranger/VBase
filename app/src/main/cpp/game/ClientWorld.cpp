#include "game/ClientWorld.h"

#include <cmath>

#include "engine/core/Input.h"
#include "engine/core/Log.h"
#include "engine/physics/CollisionWorld.h"
#include "game/BuildRules.h"        // blocksPath, footprintBox (общие с сервером)
#include "game/Character.h"
#include "game/ClientSession.h"
#include "game/CombatFx.h"          // isCombatType, combatHeadHeight, k* (общие с Scene::render)
#include "game/SceneDesc.h"
#include "game/WorldPresentation.h"

ClientWorld::ClientWorld() = default;
ClientWorld::~ClientWorld() = default;

void ClientWorld::configure(const SceneDesc& desc, Character& player) {
    collision_ = std::make_unique<CollisionWorld>();  // свежий мир коллизий
    grid_ = desc.grid;
    for (const ColliderSpec& cs : desc.colliders) {
        collision_->addBox(cs.center, cs.half);
    }
    collision_->finalize();  // оптимизация broad-phase после всей статики
    LOGI("Физика: %d статичных коллайдеров", (int)desc.colliders.size());
    // Кинематический контроллер игрока (санитизированные позиция/капсула).
    if (desc.player.present) {
        player.position = desc.player.pos;
        player.collider = collision_->addCharacter(desc.player.pos, desc.player.colliderRadius,
                                                   desc.player.colliderCylHalf);
        player.snapshot();  // prev = curr, чтобы первый кадр не «прыгнул»
    }
    pending_.clear();
    buildingColliders_.clear();
    simClock_ = 0.0;
}

void ClientWorld::recordInput(const InputCommand& cmd, float dt) {
    pending_.push_back({cmd, dt});
    // P2-03: ограничиваем окно неподтверждённых вводов. При долгом отсутствии ack (обрыв/лаг)
    // буфер рос бы без предела, а реплей реконсиляции — всё дороже. Сбрасываем самые старые:
    // следующий авторитетный снапшот всё равно поправит позицию.
    constexpr size_t kMaxPending = 256;  // ~8.5 с при 30 тиках/с
    if (pending_.size() > kMaxPending)
        pending_.erase(pending_.begin(), pending_.begin() + (pending_.size() - kMaxPending));
}

void ClientWorld::applySnapshot(Character& player, std::vector<RemoteEntity>& remotes,
                                ClientSession& session, WorldPresentation& pres,
                                const std::vector<PlayerModel>& mobs, uint8_t& localTeam,
                                float& localHp, float& localRespawn) {
    uint32_t myId = session.myId();
    if (myId == 0) return;  // ждём Welcome, иначе примем себя за чужого

    const std::vector<EntityState>& states = session.states();
    const uint32_t ack = session.ackSeq();

    for (const EntityState& s : states) {
        if (s.id == myId) {
            // Reconciliation: ставим авторитетное состояние сервера и ПЕРЕИГРЫВАЕМ
            // все вводы, которые сервер ещё не обработал (seq > ack).
            localTeam = s.team;  // своя команда — для ресурса/стройки per-team
            const float prevLocalHp = localHp;
            localHp = s.hp;      // ставки: hp своего героя (<=0 = повержен)
            localRespawn = s.aux;  // отсчёт респауна (сервер шлёт в aux при поверженном)
            // Читаемость: свой герой получил урон — вспышка + красное число (респаун = рост hp, не в счёт).
            if (prevLocalHp > 0.0f && s.hp < prevLocalHp - kDmgMinAmount) {
                const float dmg = prevLocalHp - s.hp;
                Vec3 hit{s.x, s.y + combatHeadHeight(EntityType::Hero), s.z};
                pres.damageNumbers.push_back({hit, dmg, 0.0f, {1.0f, 0.45f, 0.40f}});
                pres.sparks.push_back({hit, 0.0f, kSparkLife, pres.sparkSeed += 0x9e3779b9u});
                pres.localFlash = kFlashDur;
                pres.emitSound(SoundId::Hit);
                // Джус: свой герой под ударом — тряска (по урону, скромнее, чем у ядра).
                float amp = dmg * 0.006f;
                if (amp > 0.20f) amp = 0.20f;
                if (amp > 0.03f) { pres.shakeTime = kShakeDur; if (amp > pres.shakeAmp) pres.shakeAmp = amp; }
            }
            player.position = {s.x, s.y, s.z};
            player.facingYaw = s.yaw;
            player.speed01 = s.speed01;
            player.animParam = s.animParam;
            player.velocityY = s.velY;  // вертикаль тоже сбрасываем на серверную —
            // иначе реплей считает прыжок от чужой скорости и он дёргается.
            player.attackTime = s.attackT;  // авторитетный остаток каста (реплей переиграет триггер)
            // Синхронизируем контроллер с авторитетной позицией перед реплеем,
            // иначе collide-and-slide стартует от устаревшей внутренней позиции.
            if (collision_ && player.collider != 0) {
                collision_->setCharacterPosition(player.collider, player.position);
            }

            size_t w = 0;  // выкидываем подтверждённые (seq <= ack)
            for (size_t i = 0; i < pending_.size(); ++i) {
                if (pending_[i].cmd.seq > ack) pending_[w++] = pending_[i];
            }
            pending_.resize(w);
            for (const PendingInput& p : pending_) {
                player.simulate(p.dt, p.cmd, collision_.get());  // реплей поверх сервера
            }
            continue;
        }

        // Чужая сущность (герой/генератор/хранилище/…): в буфер интерполяции + тип.
        RemoteEntity* r = nullptr;
        for (auto& re : remotes) {
            if (re.id == s.id) { r = &re; break; }
        }
        const float prevHp = (r != nullptr) ? r->hp : 0.0f;  // до перезаписи — для дифа урона
        const bool created = (r == nullptr);                  // новая сущность в этом снапшоте
        if (r == nullptr) {
            RemoteEntity re;
            re.id = s.id;
            re.ch.position = {s.x, s.y, s.z};
            re.ch.facingYaw = s.yaw;
            remotes.push_back(re);
            r = &remotes.back();
        }
        r->type = s.type;
        r->team = s.team;
        r->charType = s.charType;  // какой моделью рисовать чужого героя (индекс ростера)
        r->aux = s.aux;  // ресурс в хранилище и т.п. (последнее значение)
        r->hp = s.hp;    // здоровье (ядро/враг)

        // Звук выстрела: снаряд появился в снапшоте (маг/башня открыли огонь).
        const EntityType cet = (EntityType)s.type;
        if (created && cet == EntityType::Projectile) pres.emitSound(SoundId::Shoot);

        // Читаемость боя: наблюдаемый максимум hp (для доли бара) + число урона/вспышка по дифу.
        if (isCombatType(cet) && s.hp > 0.0f) {
            float& mx = pres.maxHpSeen[s.id];
            if (s.hp > mx) mx = s.hp;  // спавн на полном → первое значение = максимум
        }
        if (isCombatType(cet) && prevHp > 0.0f && s.hp < prevHp - kDmgMinAmount) {
            const float dmg = prevHp - s.hp;
            const bool friendly = (cet != EntityType::Enemy) && (s.team == localTeam);
            Vec3 dcol = friendly ? Vec3{1.0f, 0.55f, 0.45f}    // урон союзнику/своему ядру — красноватый
                                 : Vec3{1.0f, 0.92f, 0.55f};   // урон врагу — жёлтый (я нанёс)
            Vec3 hit{s.x, s.y + combatHeadHeight(cet), s.z};
            pres.damageNumbers.push_back({hit, dmg, 0.0f, dcol});
            pres.sparks.push_back({hit, 0.0f, kSparkLife, pres.sparkSeed += 0x9e3779b9u});  // искры в точке хита
            pres.flash[s.id] = kFlashDur;                                               // вспышка + scale-punch
            pres.emitSound(cet == EntityType::Core ? SoundId::CoreHit : SoundId::Hit);
            // Джус: удар по ядру — тряска камеры (амплитуда по урону).
            if (cet == EntityType::Core) {
                float amp = dmg * 0.012f;
                if (amp > 0.45f) amp = 0.45f;
                if (amp > 0.05f) { pres.shakeTime = kShakeDur; if (amp > pres.shakeAmp) pres.shakeAmp = amp; }
            }
        }
        r->buffer.push_back({simClock_, {s.x, s.y, s.z}, s.yaw, s.animParam, s.attackT});
        // Ограничиваем историю (~1 сек), чтобы буфер не рос.
        while (r->buffer.size() > 2 && r->buffer[1].t < simClock_ - 1.0) {
            r->buffer.erase(r->buffer.begin());
        }
    }

    // Убрать исчезнувшие сущности (нет в текущем снапшоте). Моб, пропавший в фазе боя, —
    // это убитый враг: оставляем локальный «труп» с анимацией смерти на его месте.
    const bool playing = (session.gamePhase() == (uint8_t)GamePhase::Playing);
    const bool heroActive = session.connected() && localHp > 0.0f;  // кандидат-убийца: свой герой
    for (size_t i = 0; i < remotes.size();) {
        bool found = false;
        for (const EntityState& s : states) {
            if (s.id == remotes[i].id) { found = true; break; }
        }
        if (!found) {
            const RemoteEntity& re = remotes[i];
            if (playing && (EntityType)re.type == EntityType::Enemy) pres.emitSound(SoundId::EnemyDeath);
            if (playing && (EntityType)re.type == EntityType::Enemy && !mobs.empty()) {
                int mi = (int)((uint32_t)re.charType % (uint32_t)mobs.size());
                if (mobs[mi].deathClip >= 0 && mobs[mi].deathClipDur > 0.0f) {
                    Vec3 p = re.buffer.empty() ? re.ch.position : re.buffer.back().pos;
                    float yaw = re.buffer.empty() ? re.ch.facingYaw : re.buffer.back().yaw;
                    float ky;  // мгновенный доворот лицом к убийце (бросок death-клипа — от него)
                    if (killerYaw(remotes, player.position, heroActive, p, re.id, ky)) yaw = ky;
                    pres.dyingMobs.push_back({mi, p, yaw, 0.0f, mobs[mi].deathClipDur});
                }
            }
            pres.maxHpSeen.erase(re.id);  // чистим косметику боя вместе с сущностью
            pres.flash.erase(re.id);
            remotes.erase(remotes.begin() + (long)i);
        } else {
            ++i;
        }
    }

    // Звук исхода матча по фронту смены фазы (победа/поражение своей команды).
    uint8_t phase = session.gamePhase();
    if (phase != pres.prevPhase) {
        if (phase == (uint8_t)GamePhase::Won) pres.emitSound(SoundId::Victory);
        else if (phase == (uint8_t)GamePhase::Lost) pres.emitSound(SoundId::Defeat);
        pres.prevPhase = phase;
    }

    // Синхронизируем футпринт-коллайдеры зданий под текущий список сущностей (для предсказания героя).
    syncBuildingColliders(remotes);
    // (Отладочный снимок навигации перестраивает Scene после applySnapshot — он держит navDebug-состояние.)
}

void ClientWorld::syncBuildingColliders(const std::vector<RemoteEntity>& remotes) {
    if (!collision_) return;
    // Здания статичны: бокс ставим один раз по позиции появления, геометрия ТА ЖЕ, что у сервера
    // (общий footprintBox из game/BuildRules.h). Предикат blocksPath — тоже общий с сервером.
    for (const RemoteEntity& r : remotes) {
        if (!blocksPath((EntityType)r.type)) continue;
        if (buildingColliders_.count(r.id) != 0) continue;
        Vec3 c, hf;
        footprintBox(r.ch.position, grid_.cell, c, hf);
        uint32_t box = collision_->addBox(c, hf);
        if (box != 0) buildingColliders_[r.id] = box;
    }
    // Убрать боксы зданий, которых больше нет (разрушены / матч-рестарт / выход из сессии).
    for (auto it = buildingColliders_.begin(); it != buildingColliders_.end();) {
        bool alive = false;
        for (const RemoteEntity& r : remotes)
            if (r.id == it->first && blocksPath((EntityType)r.type)) { alive = true; break; }
        if (alive) {
            ++it;
        } else {
            collision_->removeBox(it->second);
            it = buildingColliders_.erase(it);
        }
    }
}

bool ClientWorld::killerYaw(const std::vector<RemoteEntity>& remotes, const Vec3& heroPos,
                            bool heroActive, const Vec3& mobPos, uint32_t mobId, float& outYaw) const {
    // Кандидаты в убийцы моба: локальный герой + чужие герои/башни (снаряд летит от стрелка,
    // поэтому «откуда прилетело» ≈ направление на ближайшего из них). Берём самую свежую позицию.
    float bestD2 = 1e18f;
    Vec3 bestSrc{};
    bool have = false;
    auto consider = [&](const Vec3& src) {
        float dx = src.x - mobPos.x, dz = src.z - mobPos.z;
        float d2 = dx * dx + dz * dz;
        if (d2 < bestD2) { bestD2 = d2; bestSrc = src; have = true; }
    };
    if (heroActive) consider(heroPos);  // локальный герой
    for (const RemoteEntity& r : remotes) {
        if (r.id == mobId) continue;
        EntityType t = (EntityType)r.type;
        if (t != EntityType::Hero && t != EntityType::Tower) continue;
        consider(r.buffer.empty() ? r.ch.position : r.buffer.back().pos);
    }
    if (!have) return false;
    float tx = bestSrc.x - mobPos.x, tz = bestSrc.z - mobPos.z;
    if (tx * tx + tz * tz < 1e-6f) return false;  // источник на мобе — разворачивать некуда
    outYaw = std::atan2(tx, tz);  // конвенция кода: yaw = atan2(dir.x, dir.z) — лицом к источнику
    return true;
}
