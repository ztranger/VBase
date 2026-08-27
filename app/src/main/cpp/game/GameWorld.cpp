#include "game/GameWorld.h"

#include <cmath>

#include "engine/core/Log.h"
#include "engine/physics/CollisionWorld.h"
#include "game/Grid.h"
#include "game/SceneDesc.h"

namespace {
// Параметры врагов (G2): скорость бега, капсула, дистанция «дошёл до ядра».
constexpr float kEnemySpeed = 3.0f;
constexpr float kEnemyRadius = 0.3f;
constexpr float kEnemyCylHalf = 0.3f;
constexpr float kCoreStopDist = 1.0f;
constexpr float kHeroHitRange = 1.3f;  // враг бьёт героя, если тот ближе этого

// Снаряд башни (серверная сущность): летит к цели, урон по попаданию.
constexpr float kProjSpeed = 16.0f;      // world/сек
constexpr float kProjHitRadius = 1.0f;   // радиус попадания (>= kProjSpeed*dt, чтобы не проскочить)
constexpr float kProjMaxLife = 2.5f;     // сек до самоуничтожения (если цель пропала)
constexpr float kTowerMuzzleY = 2.2f;    // высота вылета снаряда над башней
constexpr float kEnemyAimY = 0.8f;       // куда целимся по высоте (центр врага)

// Очередь ввода героя (см. HeroInputBuf). Целевая глубина буфера сглаживает джиттер;
// при бэклоге выше неё догоняем на +1 ввод/тик, чтобы буфер не превращался в постоянную
// задержку. Жёсткий предел — защита от флуда / долгой смерти (иначе очередь растёт).
constexpr size_t kInputBufferTarget = 2;
constexpr size_t kInputQueueCap = 16;

// Занимает ли тип клетку сетки (для проверки коллизии размещения).
bool isBuildingType(EntityType t) {
    return t == EntityType::Generator || t == EntityType::Storage ||
           t == EntityType::Spawner || t == EntityType::Tower || t == EntityType::Core;
}

// Враждебны ли команды (кого бой считает целью). team 0 — нейтрал/PvE: враждебен всем,
// в т.ч. сам себе — иначе чисто-PvE-сцена (всё team 0) перестала бы воевать. Стороны 1/2
// враждебны разным командам и дружественны своей (PvP: башни/враги не бьют своих).
bool hostile(uint8_t a, uint8_t b) {
    if (a == 0 && b == 0) return true;  // чистый PvE — прежнее поведение
    return a != b;
}
}  // namespace

GameWorld::GameWorld() = default;
GameWorld::~GameWorld() = default;

Entity* GameWorld::entityById(uint32_t id) {
    for (Entity& e : entities_) if (e.id == id) return &e;
    return nullptr;
}
const Entity* GameWorld::entityById(uint32_t id) const {
    for (const Entity& e : entities_) if (e.id == id) return &e;
    return nullptr;
}

void GameWorld::reset() {
    entities_.clear();
    heroInputs_.clear();
    world_.reset();  // контроллеры уйдут вместе с миром
    nextEntityId_ = 1;
    for (float& r : resourcePerTeam_) r = 0.0f;
    spawnPos_ = Vec3{0.0f, 0.0f, 0.0f};
    capsuleRadius_ = 0.3f;
    capsuleCylHalf_ = 0.3f;
    enemyStats_ = EnemySpec{};
    enemyTypes_.clear();
    heroTypes_.clear();
    for (BuildTemplate& t : buildTemplates_) t = BuildTemplate{};
    heroHp_ = 100.0f;
    heroRespawn_ = 5.0f;
    grid_ = Grid{};
    for (int t = 0; t < kMaxTeams; ++t) {
        spawnByTeam_[t] = Vec3{0.0f, 0.0f, 0.0f};
        spawnValid_[t] = false;
        coreMaxByTeam_[t] = 0.0f;
        coreHpByTeam_[t] = 0.0f;
    }
    decided_ = false;
    waveCleared_ = false;
    buildingSpecs_.clear();
    matchRestartDelay_ = 0.0f;
    restartTimer_ = 0.0f;
}

void GameWorld::configure(const SceneDesc& desc) {
    reset();  // идемпотентно: свежий мир на каждую конфигурацию
    world_ = std::make_unique<CollisionWorld>();
    for (const ColliderSpec& cs : desc.colliders) {
        world_->addBox(cs.center, cs.half);
    }
    world_->finalize();
    spawnPos_ = desc.player.pos;
    capsuleRadius_ = desc.player.colliderRadius;
    capsuleCylHalf_ = desc.player.colliderCylHalf;
    enemyStats_ = desc.enemy;  // дефолтные статы врага (fallback, если нет типов)
    enemyTypes_ = desc.enemyTypes;  // статы по типу моба (индекс = charType); пусто -> все по enemyStats_
    heroTypes_ = desc.heroTypes;    // статы по типу героя (hp/speed); пусто -> дефолт (heroHp_/6)
    for (int i = 0; i < 8; ++i) buildTemplates_[i] = desc.build[i];  // шаблоны построек героя
    heroHp_ = desc.player.hp;
    heroRespawn_ = desc.player.respawnDelay > 0.0f ? desc.player.respawnDelay : 5.0f;
    grid_ = desc.grid;  // строительная сетка из сцены (та же, что у клиента)
    matchRestartDelay_ = desc.matchRestartDelay;  // 0 = после исхода мир стоит до перезапуска сервера

    // Точки спавна сторон (PvP). Играбельны только команды с заданной точкой. Нет ни одной —
    // fallback: команда 0 спавнит в player.pos (соло/кооп ведут себя как раньше).
    for (const SpawnSpec& s : desc.spawns)
        if (s.team < kMaxTeams) { spawnByTeam_[s.team] = s.pos; spawnValid_[s.team] = true; }
    bool anySpawn = false;
    for (bool v : spawnValid_) anySpawn = anySpawn || v;
    if (!anySpawn) { spawnByTeam_[0] = spawnPos_; spawnValid_[0] = true; }

    // Здания базы: сохраняем спеки (для матч-рестарта — пересоздать базы) и спавним.
    buildingSpecs_ = desc.buildings;
    spawnBuildings();
    LOGI("GameWorld: мир — %d коллайдеров, %d сущностей базы", (int)desc.colliders.size(),
         (int)desc.buildings.size());
}

void GameWorld::spawnBuildings() {
    // Инстанцирует сущности базы из buildingSpecs_ (генератор/хранилище/спавнер/башня/ядро).
    for (const BuildingSpec& b : buildingSpecs_) {
        Entity e;
        e.id = nextEntityId_++;
        switch (b.kind) {
            case BuildingSpec::Generator: e.type = EntityType::Generator; break;
            case BuildingSpec::Storage:   e.type = EntityType::Storage;   break;
            case BuildingSpec::Spawner:   e.type = EntityType::Spawner;   break;
            case BuildingSpec::Tower:     e.type = EntityType::Tower;     break;
            case BuildingSpec::Core:      e.type = EntityType::Core;      break;
        }
        e.team = b.team;        // сторона (соло/кооп = 0; PvP 1/2)
        e.move.position = b.pos;
        e.move.snapshot();
        e.rate = b.rate;        // generator/spawner/tower: интервал/скорость
        e.cap = b.cap;          // storage/spawner
        e.hp = e.maxHp = b.hp;  // core: здоровье
        e.damage = b.damage;    // tower: урон
        e.range = b.range;      // tower: радиус
        entities_.push_back(e);
    }
}

void GameWorld::restartMatch() {
    // Пересобрать матч, НЕ трогая геометрию/конфиг/подключения: убрать здания и врагов, заново
    // заспавнить базы, воскресить героев в их точках, обнулить экономику и исход. Игроки остаются.
    for (size_t i = 0; i < entities_.size();) {
        Entity& e = entities_[i];
        if (e.type == EntityType::Hero) { ++i; continue; }  // герои остаются (игроки подключены)
        if (world_ && e.move.collider != 0) world_->removeCharacter(e.move.collider);  // враг
        entities_.erase(entities_.begin() + (long)i);
    }
    spawnBuildings();  // свежие базы (новые id — клиент увидит их как новые сущности)

    for (float& r : resourcePerTeam_) r = 0.0f;
    for (Entity& e : entities_) {
        if (e.type != EntityType::Hero) continue;
        const Vec3 sp = spawnFor(e.team);
        e.hp = e.maxHp = heroHp_;
        e.aux = 0.0f;
        e.move.position = sp;
        e.move.velocityY = 0.0f;
        e.move.snapshot();
        if (world_ && e.move.collider != 0) world_->setCharacterPosition(e.move.collider, sp);
    }

    decided_ = false;
    waveCleared_ = false;
    restartTimer_ = 0.0f;
    for (int t = 0; t < kMaxTeams; ++t) { coreMaxByTeam_[t] = 0.0f; coreHpByTeam_[t] = 0.0f; }
    LOGI("GameWorld: матч перезапущен");
}

Vec3 GameWorld::spawnFor(uint8_t team) const {
    return (team < kMaxTeams && spawnValid_[team]) ? spawnByTeam_[team] : spawnPos_;
}

uint32_t GameWorld::addPlayer() {
    // Наименее населённая ИГРАБЕЛЬНАЯ команда (у неё есть точка спавна). PvP балансирует 1/2;
    // соло/кооп — все в единственную валидную (team 0). Балансировка по числу живых героев.
    int count[kMaxTeams] = {0};
    for (const Entity& e : entities_)
        if (e.type == EntityType::Hero && e.team < kMaxTeams) count[e.team]++;
    int best = -1;
    for (int t = 0; t < kMaxTeams; ++t) {
        if (!spawnValid_[t]) continue;
        if (best < 0 || count[t] < count[best]) best = t;
    }
    return addHero(best < 0 ? 0 : (uint8_t)best);
}

uint32_t GameWorld::addHero(uint8_t team) {
    const Vec3 sp = spawnFor(team);
    Entity hero;
    hero.id = nextEntityId_++;
    hero.type = EntityType::Hero;
    hero.team = team;
    hero.hp = hero.maxHp = heroHp_;  // ставки: героя можно повергнуть
    hero.move.position = sp;
    hero.move.snapshot();
    if (world_) {
        hero.move.collider = world_->addCharacter(sp, capsuleRadius_, capsuleCylHalf_);
    }
    entities_.push_back(hero);
    return hero.id;
}

void GameWorld::removeEntity(uint32_t id) {
    Entity* e = entityById(id);
    if (e != nullptr && world_ && e->move.collider != 0) {
        world_->removeCharacter(e->move.collider);
    }
    heroInputs_.erase(id);  // очередь ввода уходит вместе с героем
    for (size_t i = 0; i < entities_.size(); ++i)
        if (entities_[i].id == id) {
            entities_.erase(entities_.begin() + (long)i);
            break;
        }
}

void GameWorld::setHeroInput(uint32_t heroId, const InputCommand& in) {
    if (entityById(heroId) == nullptr) return;  // неизвестный герой — игнор
    HeroInputBuf& b = heroInputs_[heroId];
    b.queue.push_back(in);
    // Переполнение (флуд / долгая смерть): старые считаем «обработанными» — двигаем ack
    // вперёд и дропаем, иначе очередь растёт, а клиентский pending не сходится.
    while (b.queue.size() > kInputQueueCap) {
        b.lastSeq = b.queue.front().seq;
        b.queue.pop_front();
    }
}

void GameWorld::setHeroCharType(uint32_t heroId, uint8_t charType) {
    Entity* e = entityById(heroId);
    if (e == nullptr) return;
    // Статы выбранного персонажа применяем при ПЕРВОМ получении charType и при его смене
    // (charType по умолчанию 0 совпал бы с выбором мага — потому отдельный флаг applied).
    if (!e->heroStatsApplied || e->charType != charType) {
        e->charType = charType;
        e->heroStatsApplied = true;
        if (e->type == EntityType::Hero) applyHeroStats(*e);
    }
}

// hp/скорость выбранного персонажа героя (config/characters.cfg по charType). Ставит полный hp
// (первый выбор / смена персонажа). Нет типа/поля -> дефолт: heroHp_ и Character.maxSpeed.
void GameWorld::applyHeroStats(Entity& e) {
    float hp = heroHp_, spd = 0.0f;  // spd 0 -> оставить дефолт move.maxSpeed
    if (e.charType < heroTypes_.size()) {
        const CharacterDesc& t = heroTypes_[e.charType];
        if (t.hp > 0.0f) hp = t.hp;
        if (t.speed > 0.0f) spd = t.speed;
    }
    e.maxHp = hp;
    e.hp = hp;  // полный hp при выборе персонажа
    if (spd > 0.0f) e.move.maxSpeed = spd;
}

bool GameWorld::tryBuild(uint32_t builderId, EntityType type, int cellX, int cellZ) {
    if ((int)type < 0 || (int)type >= 8) return false;
    const BuildTemplate& t = buildTemplates_[(int)type];
    if (!t.buildable) return false;          // тип нельзя ставить (нет cost в конфиге)

    uint8_t team = 0;  // команда строителя — из неё тратим ресурс и ей же принадлежит здание
    const Entity* builder = entityById(builderId);
    if (builder != nullptr) team = builder->team;
    if (team >= kMaxTeams) return false;
    if (resourcePerTeam_[team] < t.cost) return false;  // не хватает ресурса команды

    if (!grid_.inArena(cellX, cellZ)) return false;  // вне зоны строительства
    for (const Entity& e : entities_) {      // клетка занята другим зданием?
        if (!isBuildingType(e.type)) continue;
        if (grid_.cellOf(e.move.position.x) == cellX && grid_.cellOf(e.move.position.z) == cellZ)
            return false;
    }

    resourcePerTeam_[team] -= t.cost;
    Entity e;
    e.id = nextEntityId_++;
    e.type = type;
    e.team = team;
    e.move.position = grid_.cellCenter(cellX, cellZ);
    e.move.snapshot();
    e.rate = t.rate;
    e.cap = t.cap;
    e.hp = e.maxHp = t.hp;
    e.damage = t.damage;
    e.range = t.range;
    // Пока БЕЗ футпринт-коллайдера: враги — steer-to-target без пасфайндинга, блок бы их
    // стопорил. Башня влияет позицией/радиусом, генератор/хранилище — экономикой.
    entities_.push_back(e);
    LOGI("GameWorld: возведено type=%d team=%d в клетке (%d,%d), ресурс=%.0f",
         (int)type, (int)team, cellX, cellZ, (double)resourcePerTeam_[team]);
    return true;
}

void GameWorld::step(float dt) {
    // Герои движутся всегда (в т.ч. после конца матча — по базе можно ходить): тот же код
    // симуляции, что предсказывает клиент. Ввод берём из FIFO-очереди — по одному за тик,
    // чтобы пачка вводов за один poll не коалесилась (иначе терялся прыжок и был rubber-band).
    for (Entity& e : entities_) {
        if (e.type != EntityType::Hero) continue;
        HeroInputBuf& b = heroInputs_[e.id];
        if (e.hp <= 0.0f) {
            // Повержён: не двигаем. Очередь осушаем, ack двигаем на последний присланный seq
            // (клиент во время смерти шлёт нули) — иначе за секунды смерти копится бэклог.
            if (!b.queue.empty()) {
                b.lastSeq = b.queue.back().seq;
                b.queue.clear();
            }
            e.move.attackTime = 0.0f;  // прерываем каст (иначе застрянет мёртвым в позе)
            continue;
        }
        e.move.snapshot();
        // Обычно 1 ввод/тик. При накопившемся бэклоге (хитч/джиттер клиента) догоняем на +1,
        // применяя вводы как отдельные суб-шаги — тем же multi-call simulate, что и реплей на
        // клиенте (безопасно: реконсилируемое состояние сбрасывается снапшотом перед реплеем).
        size_t take = b.queue.empty() ? 0 : (b.queue.size() > kInputBufferTarget ? 2u : 1u);
        if (take == 0) {
            // Очередь пуста (пакет опоздал/потерян): удерживаем последний ввод без повторного
            // прыжка — движение продолжается, как раньше делал «залипший» hero->input.
            InputCommand hold = b.hasLast ? b.last : InputCommand{};
            hold.jump = false;
            e.move.simulate(dt, hold, world_.get());
        } else {
            for (size_t k = 0; k < take; ++k) {
                InputCommand in = b.queue.front();
                b.queue.pop_front();
                b.last = in;
                b.lastSeq = in.seq;
                b.hasLast = true;
                e.input = in;  // отражаем применённый ввод (диагностика)
                e.move.simulate(dt, in, world_.get());  // прыжок потреблён вместе с этим вводом
            }
        }
        // Отсчёт таймера атаки — РОВНО 1 раз за тик (не в simulate: multi-call реплей/бэклог
        // досрочно завершал бы каст). Триггер каста ставится в simulate, отсчёт — здесь.
        if (e.move.attackTime > 0.0f) {
            e.move.attackTime -= dt;
            if (e.move.attackTime < 0.0f) e.move.attackTime = 0.0f;
        }
    }

    if (decided_) {  // матч завершён — экономика/бой стоят (герои двигаются выше)
        if (matchRestartDelay_ > 0.0f) {  // авто-рестарт включён сценой
            restartTimer_ -= dt;
            if (restartTimer_ <= 0.0f) restartMatch();
        }
        return;
    }

    // Экономика ПО КОМАНДАМ: у каждой стороны свой пул, питаемый её генераторами и
    // ограниченный суммой ёмкостей её хранилищ (лишнее теряется — потолок накоплений).
    // Пул раскладываем по хранилищам этой команды в aux — так он попадает в сеть.
    {
        float prod[kMaxTeams] = {0.0f}, cap[kMaxTeams] = {0.0f};
        for (const Entity& e : entities_) {
            if (e.team >= kMaxTeams) continue;
            if (e.type == EntityType::Generator) prod[e.team] += e.rate;
            else if (e.type == EntityType::Storage) cap[e.team] += e.cap;
        }
        float rem[kMaxTeams];
        for (int t = 0; t < kMaxTeams; ++t) {
            resourcePerTeam_[t] += prod[t] * dt;
            if (resourcePerTeam_[t] > cap[t]) resourcePerTeam_[t] = cap[t];
            if (resourcePerTeam_[t] < 0.0f) resourcePerTeam_[t] = 0.0f;
            rem[t] = resourcePerTeam_[t];
        }
        for (Entity& e : entities_) {
            if (e.type != EntityType::Storage || e.team >= kMaxTeams) continue;
            float amt = rem[e.team] < e.cap ? rem[e.team] : e.cap;
            e.aux = amt;
            rem[e.team] -= amt;
        }
    }

    // Спавнеры: по таймеру плодят врагов (до потолка cap) с боевыми статами из конфига.
    // Новые сущности КОПИМ и добавляем ПОСЛЕ цикла — push_back инвалидировал бы итерацию.
    {
        std::vector<Entity> spawned;
        for (Entity& e : entities_) {
            if (e.type != EntityType::Spawner) continue;
            if (e.rate <= 0.0f || e.spawnedCount >= (int)e.cap) continue;
            e.timer += dt;
            if (e.timer >= e.rate) {
                e.timer -= e.rate;
                Entity enemy;
                enemy.id = nextEntityId_++;
                enemy.type = EntityType::Enemy;
                enemy.team = e.team;
                // Тип моба: чередуем по спавну. Индекс = charType (клиент берёт mobs_[charType %
                // size]); он же выбирает статы из enemyTypes_ (config/enemies.cfg).
                int nTypes = (int)enemyTypes_.size();
                int type = (nTypes > 0) ? (e.spawnedCount % nTypes) : (e.spawnedCount % 4);
                enemy.charType = (uint8_t)type;
                enemy.move.position = e.move.position;
                enemy.move.snapshot();
                // Статы: из типа (enemyTypes_), с откатом на дефолтные enemyStats_/kEnemySpeed.
                float ehp = enemyStats_.hp, edmg = enemyStats_.damage;
                float espeed = kEnemySpeed, eatk = enemyStats_.attackInterval;
                if (nTypes > 0) {
                    const CharacterDesc& ct = enemyTypes_[type];
                    if (ct.hp > 0.0f) ehp = ct.hp;
                    if (ct.damage > 0.0f) edmg = ct.damage;
                    if (ct.speed > 0.0f) espeed = ct.speed;
                    if (ct.attackInterval > 0.0f) eatk = ct.attackInterval;
                }
                enemy.hp = enemy.maxHp = ehp;
                enemy.damage = edmg;
                enemy.move.maxSpeed = espeed;  // скорость бега (используется в движении врага)
                enemy.rate = eatk;             // для врага rate = интервал удара
                if (world_)
                    enemy.move.collider = world_->addCharacter(
                        e.move.position, kEnemyRadius, kEnemyCylHalf);
                spawned.push_back(enemy);
                e.spawnedCount++;
            }
        }
        for (Entity& n : spawned) entities_.push_back(std::move(n));
    }

    // Типизированные списки одним O(N)-проходом — чтобы боевые системы НЕ сканировали весь
    // вектор на каждой сущности (было O(N²): каждый враг искал ядра/героев по всем entities_,
    // каждая башня — врагов). entities_ стабилен от спавна выше до уборки трупов ниже —
    // указатели живут. Ядер/героев/башен немного, врагов может быть много: теперь бой ~O(N).
    std::vector<Entity*> cores, heroes, enemies;
    for (Entity& e : entities_) {
        if (e.type == EntityType::Core) cores.push_back(&e);
        else if (e.type == EntityType::Hero) heroes.push_back(&e);
        else if (e.type == EntityType::Enemy) enemies.push_back(&e);
    }

    // Враги бегут к ближайшему ВРАЖДЕБНОМУ ядру (в PvP у каждой стороны своё — враг бежит на
    // чужое; в PvE всё team 0 и hostile(0,0)=true, поведение как раньше), а по кулдауну бьют
    // БЛИЖАЙШУЮ цель в радиусе: враждебное ядро (kCoreStopDist) ИЛИ живого враждебного героя
    // (kHeroHitRange). Цель зависит от команды врага — считаем пер-врага.
    for (Entity* ep : enemies) {
        Entity& e = *ep;
        // Ближайшее враждебное ядро — цель движения.
        Entity* core = nullptr;
        float coreDist = 1e30f;
        for (Entity* cp : cores) {
            if (!hostile(e.team, cp->team)) continue;
            Vec3 to = cp->move.position - e.move.position;
            to.y = 0.0f;
            float d = std::sqrt(to.x * to.x + to.z * to.z);
            if (d < coreDist) { coreDist = d; core = cp; }
        }
        Vec3 vel{0.0f, 0.0f, 0.0f};
        if (core != nullptr && coreDist > kCoreStopDist) {
            Vec3 to = core->move.position - e.move.position;
            to.y = 0.0f;
            Vec3 dir = to * (1.0f / coreDist);
            float spd = e.move.maxSpeed > 0.0f ? e.move.maxSpeed : kEnemySpeed;  // per-type скорость
            vel = dir * spd;
            e.move.facingYaw = std::atan2(dir.x, dir.z);
        }
        // Цель удара — ближайшее в радиусе: враждебное ядро (в упоре) или ближайший живой
        // враждебный герой.
        Entity* atk = nullptr;
        float atkDist = 1e30f;
        if (core != nullptr && coreDist <= kCoreStopDist) { atk = core; atkDist = coreDist; }
        for (Entity* hero : heroes) {
            if (hero->hp <= 0.0f || !hostile(e.team, hero->team)) continue;
            Vec3 d = hero->move.position - e.move.position;
            d.y = 0.0f;
            float hd = std::sqrt(d.x * d.x + d.z * d.z);
            if (hd <= kHeroHitRange && hd < atkDist) { atk = hero; atkDist = hd; }
        }
        if (atk != nullptr) {
            e.timer += dt;
            float atkInt = e.rate > 0.0f ? e.rate : enemyStats_.attackInterval;  // per-type интервал
            if (atkInt > 0.0f && e.timer >= atkInt) {
                e.timer -= atkInt;
                atk->hp -= e.damage;
            }
        } else {
            e.timer = 0.0f;  // никого в радиусе — таймер удара сброшен (подход не бьёт мгновенно)
        }
        // Флаг «в упоре и бьёт цель» -> клиент лупит attack-клип моба (иначе walk). Пишется в
        // EntityState.attackT (то же поле, что каст героя) — без бампа протокола.
        e.move.attackTime = (atk != nullptr) ? 1.0f : 0.0f;
        if (world_ && e.move.collider != 0)
            e.move.position =
                world_->moveCharacter(e.move.collider, vel, e.move.velocityY, false, dt);
        else
            e.move.position = e.move.position + vel * dt;
    }

    // Башни: по кулдауну (rate) выпускают СНАРЯД в ближайшего врага в радиусе (range). Урон
    // применяется при попадании (см. система снарядов ниже), а не мгновенно.
    std::vector<Entity> projSpawned;  // добавим ПОСЛЕ цикла — push_back инвалидировал бы итерацию
    for (Entity& tw : entities_) {
        if (tw.type != EntityType::Tower) continue;
        tw.timer += dt;
        if (tw.rate <= 0.0f || tw.timer < tw.rate) continue;
        Entity* target = nullptr;
        float bestD2 = tw.range * tw.range;
        for (Entity* enp : enemies) {  // список из прохода выше (без скана всего вектора на башню)
            if (enp->hp <= 0.0f || !hostile(tw.team, enp->team)) continue;
            Vec3 d = enp->move.position - tw.move.position;
            float d2 = d.x * d.x + d.y * d.y + d.z * d.z;
            if (d2 <= bestD2) { bestD2 = d2; target = enp; }
        }
        if (target != nullptr) {
            tw.timer -= tw.rate;
            Entity proj;
            proj.id = nextEntityId_++;
            proj.type = EntityType::Projectile;
            proj.team = tw.team;
            proj.move.position = tw.move.position + Vec3{0.0f, kTowerMuzzleY, 0.0f};
            proj.move.snapshot();
            proj.damage = tw.damage;
            proj.move.maxSpeed = kProjSpeed;
            proj.targetId = target->id;  // хоминг по id цели
            Vec3 to = (target->move.position + Vec3{0.0f, kEnemyAimY, 0.0f}) - proj.move.position;
            proj.move.facingYaw = std::atan2(to.x, to.z);  // начальный курс (для ориентации болта)
            projSpawned.push_back(std::move(proj));
        } else {
            tw.timer = tw.rate;  // цели нет — держим заряд готовым (без роста таймера)
        }
    }
    for (Entity& p : projSpawned) entities_.push_back(std::move(p));

    // Снаряды: хомингом летят к цели (по её id). Попал в радиусе — урон и самоуничтожение;
    // цель пропала — летят прямо по последнему курсу и гаснут по kProjMaxLife (уборка ниже).
    for (Entity& p : entities_) {
        if (p.type != EntityType::Projectile) continue;
        p.timer += dt;
        Entity* tgt = entityById(p.targetId);
        Vec3 dir;
        if (tgt != nullptr && tgt->type == EntityType::Enemy && tgt->hp > 0.0f) {
            Vec3 to = (tgt->move.position + Vec3{0.0f, kEnemyAimY, 0.0f}) - p.move.position;
            float d = std::sqrt(to.x * to.x + to.y * to.y + to.z * to.z);
            if (d <= kProjHitRadius) {          // попадание
                tgt->hp -= p.damage;
                p.timer = kProjMaxLife;         // пометить на уборку
                continue;
            }
            dir = (d > 1e-4f) ? to * (1.0f / d) : Vec3{0.0f, 0.0f, 1.0f};
            p.move.facingYaw = std::atan2(dir.x, dir.z);
        } else {
            dir = Vec3{std::sin(p.move.facingYaw), 0.0f, std::cos(p.move.facingYaw)};
        }
        float spd = p.move.maxSpeed > 0.0f ? p.move.maxSpeed : kProjSpeed;
        p.move.position = p.move.position + dir * (spd * dt);
    }

    // Ставки героя: при hp<=0 герой повержен. Таймер респауна храним в aux (едет клиенту как
    // обратный отсчёт), по истечении — возрождение в точке спавна с полным hp.
    for (Entity& e : entities_) {
        if (e.type != EntityType::Hero || e.hp > 0.0f) continue;  // жив — пропускаем
        if (e.aux <= 0.0f) {
            e.aux = heroRespawn_;  // только что повержен — запустить отсчёт
        } else {
            e.aux -= dt;
            if (e.aux <= 0.0f) {  // отсчёт вышел — возрождаем в спавн-точке своей команды
                const Vec3 sp = spawnFor(e.team);
                e.hp = e.maxHp;
                e.aux = 0.0f;
                e.move.position = sp;
                e.move.velocityY = 0.0f;
                e.move.snapshot();
                if (world_ && e.move.collider != 0)
                    world_->setCharacterPosition(e.move.collider, sp);
            }
        }
    }

    // Уборка убитых врагов (hp<=0): удаляем сущность и её контроллер. Делаем ПОСЛЕ всех
    // систем — тут указатель core инвалидируется, дальше им не пользуемся.
    for (size_t i = 0; i < entities_.size();) {
        Entity& e = entities_[i];
        bool deadEnemy = (e.type == EntityType::Enemy && e.hp <= 0.0f);
        bool deadProj = (e.type == EntityType::Projectile && e.timer >= kProjMaxLife);
        if (deadEnemy || deadProj) {
            if (world_ && e.move.collider != 0) world_->removeCharacter(e.move.collider);
            entities_.erase(entities_.begin() + (long)i);
        } else {
            ++i;
        }
    }

    // Жизненный цикл матча — PER-TEAM. Считаем ядра по командам (maxHp>0 — иначе ядро-плейсхолдер
    // считалось бы разрушенным сразу) и статус волн; кэшируем для phaseForTeam. Матч завершён,
    // когда какая-то core-команда уничтожена (PvP/PvE-поражение) ИЛИ одна сторона отбила все волны.
    for (int t = 0; t < kMaxTeams; ++t) { coreMaxByTeam_[t] = 0.0f; coreHpByTeam_[t] = 0.0f; }
    bool haveSpawner = false, allSpawned = true;
    int enemyCount = 0;
    for (const Entity& e : entities_) {
        if (e.type == EntityType::Core && e.team < kMaxTeams && e.maxHp > 0.0f) {
            coreMaxByTeam_[e.team] += e.maxHp;
            coreHpByTeam_[e.team] += (e.hp > 0.0f ? e.hp : 0.0f);
        } else if (e.type == EntityType::Spawner) {
            haveSpawner = true;
            if (e.spawnedCount < (int)e.cap) allSpawned = false;
        } else if (e.type == EntityType::Enemy) {
            ++enemyCount;
        }
    }
    waveCleared_ = haveSpawner && allSpawned && enemyCount == 0;

    int coreTeams = 0, aliveCoreTeams = 0;
    for (int t = 0; t < kMaxTeams; ++t)
        if (coreMaxByTeam_[t] > 0.0f) { ++coreTeams; if (coreHpByTeam_[t] > 0.0f) ++aliveCoreTeams; }
    const bool anyEliminated = coreTeams > aliveCoreTeams;   // хотя бы одно ядро-команда пало
    const bool pveWin = coreTeams <= 1 && waveCleared_;      // одна сторона отбила все волны
    if ((anyEliminated || pveWin) && !decided_) {
        decided_ = true;
        restartTimer_ = matchRestartDelay_;  // >0 → авто-рестарт по таймеру; 0 → мир стоит
        LOGI("GameWorld: матч завершён (ядро-команд %d, живых %d, волны %s)%s", coreTeams,
             aliveCoreTeams, waveCleared_ ? "отбиты" : "нет",
             matchRestartDelay_ > 0.0f ? " — будет авто-рестарт" : "");
    }
}

GamePhase GameWorld::phaseForTeam(uint8_t t) const {
    if (t >= kMaxTeams) return GamePhase::Playing;
    if (coreMaxByTeam_[t] > 0.0f && coreHpByTeam_[t] <= 0.0f) return GamePhase::Lost;  // моё ядро пало
    int otherCore = 0, otherAlive = 0;
    for (int k = 0; k < kMaxTeams; ++k) {
        if (k == (int)t || coreMaxByTeam_[k] <= 0.0f) continue;
        ++otherCore;
        if (coreHpByTeam_[k] > 0.0f) ++otherAlive;
    }
    if (otherCore > 0 && otherAlive == 0) return GamePhase::Won;  // все чужие ядра пали (PvP)
    if (otherCore == 0 && waveCleared_) return GamePhase::Won;    // PvE: волны отбиты
    return GamePhase::Playing;
}

uint8_t GameWorld::teamOf(uint32_t id) const {
    const Entity* e = entityById(id);
    return e != nullptr ? e->team : 0;
}

void GameWorld::writeStates(std::vector<EntityState>& out) const {
    out.resize(entities_.size());
    for (size_t i = 0; i < entities_.size(); ++i) {
        const Entity& e = entities_[i];
        EntityState& s = out[i];
        s.id = e.id;
        s.type = (uint8_t)e.type;
        s.team = e.team;
        s.x = e.move.position.x;
        s.y = e.move.position.y;
        s.z = e.move.position.z;
        s.yaw = e.move.facingYaw;
        s.animParam = e.move.animParam;
        s.speed01 = e.move.speed01;
        s.velY = e.move.velocityY;
        s.hp = e.hp;
        s.aux = e.aux;
        s.attackT = e.move.attackTime;
        s.charType = e.charType;
    }
}

uint32_t GameWorld::inputSeq(uint32_t heroId) const {
    // ackSeq = последний ПОТРЕБЛЁННЫЙ ввод (не последний присланный) — иначе клиент выкинет
    // из pending вводы, которые сервер ещё не симулировал, и своего аватара откинет назад.
    auto it = heroInputs_.find(heroId);
    return it != heroInputs_.end() ? it->second.lastSeq : 0;
}

int GameWorld::enemyCount() const {
    int n = 0;
    for (const Entity& e : entities_) if (e.type == EntityType::Enemy) ++n;
    return n;
}

float GameWorld::coreHp() const {
    for (const Entity& e : entities_) if (e.type == EntityType::Core) return e.hp;
    return 0.0f;
}

float GameWorld::heroHp(uint32_t id) const {
    const Entity* e = entityById(id);
    return (e != nullptr && e->type == EntityType::Hero) ? e->hp : -1.0f;
}

Vec3 GameWorld::heroPos(uint32_t id) const {
    const Entity* e = entityById(id);
    return e != nullptr ? e->move.position : Vec3{0.0f, 0.0f, 0.0f};
}
