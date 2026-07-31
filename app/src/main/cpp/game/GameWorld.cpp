#include "game/GameWorld.h"

#include <cmath>

#include "engine/core/Log.h"
#include "engine/physics/CollisionWorld.h"
#include "game/SceneDesc.h"

namespace {
// Параметры врагов (G2): скорость бега, капсула, дистанция «дошёл до ядра».
constexpr float kEnemySpeed = 3.0f;
constexpr float kEnemyRadius = 0.3f;
constexpr float kEnemyCylHalf = 0.3f;
constexpr float kCoreStopDist = 1.0f;
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
    world_.reset();  // контроллеры уйдут вместе с миром
    nextEntityId_ = 1;
    resource_ = 0.0f;
    spawnPos_ = Vec3{0.0f, 0.0f, 0.0f};
    capsuleRadius_ = 0.3f;
    capsuleCylHalf_ = 0.3f;
    enemyStats_ = EnemySpec{};
    phase_ = GamePhase::Playing;
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
    enemyStats_ = desc.enemy;  // hp/урон/интервал врага (враг не в сцене — плодит спавнер)

    // Спавним статичные сущности базы из описания (генератор/хранилище/спавнер/башня/ядро).
    for (const BuildingSpec& b : desc.buildings) {
        Entity e;
        e.id = nextEntityId_++;
        switch (b.kind) {
            case BuildingSpec::Generator: e.type = EntityType::Generator; break;
            case BuildingSpec::Storage:   e.type = EntityType::Storage;   break;
            case BuildingSpec::Spawner:   e.type = EntityType::Spawner;   break;
            case BuildingSpec::Tower:     e.type = EntityType::Tower;     break;
            case BuildingSpec::Core:      e.type = EntityType::Core;      break;
        }
        e.move.position = b.pos;
        e.move.snapshot();
        e.rate = b.rate;        // generator/spawner/tower: интервал/скорость
        e.cap = b.cap;          // storage/spawner
        e.hp = e.maxHp = b.hp;  // core: здоровье
        e.damage = b.damage;    // tower: урон
        e.range = b.range;      // tower: радиус
        entities_.push_back(e);
    }
    LOGI("GameWorld: мир — %d коллайдеров, %d сущностей базы", (int)desc.colliders.size(),
         (int)desc.buildings.size());
}

uint32_t GameWorld::addHero(uint8_t team) {
    Entity hero;
    hero.id = nextEntityId_++;
    hero.type = EntityType::Hero;
    hero.team = team;
    hero.move.position = spawnPos_;
    hero.move.snapshot();
    if (world_) {
        hero.move.collider = world_->addCharacter(spawnPos_, capsuleRadius_, capsuleCylHalf_);
    }
    entities_.push_back(hero);
    return hero.id;
}

void GameWorld::removeEntity(uint32_t id) {
    Entity* e = entityById(id);
    if (e != nullptr && world_ && e->move.collider != 0) {
        world_->removeCharacter(e->move.collider);
    }
    for (size_t i = 0; i < entities_.size(); ++i)
        if (entities_[i].id == id) {
            entities_.erase(entities_.begin() + (long)i);
            break;
        }
}

void GameWorld::setHeroInput(uint32_t heroId, const InputCommand& in) {
    Entity* hero = entityById(heroId);
    if (hero != nullptr) hero->input = in;
}

void GameWorld::step(float dt) {
    // Герои движутся всегда (в т.ч. после конца матча — по базе можно ходить): тот же код
    // симуляции, что предсказывает клиент.
    for (Entity& e : entities_) {
        if (e.type == EntityType::Hero) {
            e.move.snapshot();
            e.move.simulate(dt, e.input, world_.get());
            e.input.jump = false;  // прыжок одноразовый
        }
    }

    if (phase_ != GamePhase::Playing) return;  // матч кончился — экономика/бой стоят

    // Экономика: генераторы капают в общий пул, ограниченный суммой ёмкостей хранилищ
    // (лишнее теряется — это и есть потолок накоплений). Пул раскладываем по хранилищам
    // в поле aux (заполнение конкретного хранилища) — так он попадает в сеть.
    {
        float prod = 0.0f, cap = 0.0f;
        for (const Entity& e : entities_) {
            if (e.type == EntityType::Generator) prod += e.rate;
            else if (e.type == EntityType::Storage) cap += e.cap;
        }
        resource_ += prod * dt;
        if (resource_ > cap) resource_ = cap;
        if (resource_ < 0.0f) resource_ = 0.0f;
        float rem = resource_;
        for (Entity& e : entities_) {
            if (e.type != EntityType::Storage) continue;
            float amt = rem < e.cap ? rem : e.cap;
            e.aux = amt;
            rem -= amt;
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
                enemy.move.position = e.move.position;
                enemy.move.snapshot();
                enemy.hp = enemy.maxHp = enemyStats_.hp;  // боевые статы врага
                enemy.damage = enemyStats_.damage;        // урон по ядру
                if (world_)
                    enemy.move.collider = world_->addCharacter(
                        e.move.position, kEnemyRadius, kEnemyCylHalf);
                spawned.push_back(enemy);
                e.spawnedCount++;
            }
        }
        for (Entity& n : spawned) entities_.push_back(std::move(n));
    }

    // Ядро базы (первое Core) — цель врагов. Указатель валиден до уборки трупов ниже
    // (враги ещё не удаляются, ядро не удаляется).
    Entity* core = nullptr;
    for (Entity& e : entities_)
        if (e.type == EntityType::Core) { core = &e; break; }

    // Враги бегут к ядру тем же кинематическим контроллером (гравитация + скольжение), а
    // добежав (в пределах kCoreStopDist) — бьют его по кулдауну (enemyStats_.attackInterval).
    if (core != nullptr) {
        for (Entity& e : entities_) {
            if (e.type != EntityType::Enemy) continue;
            Vec3 to = core->move.position - e.move.position;
            to.y = 0.0f;
            float dist = std::sqrt(to.x * to.x + to.z * to.z);
            Vec3 vel{0.0f, 0.0f, 0.0f};
            if (dist > kCoreStopDist) {
                Vec3 dir = to * (1.0f / dist);
                vel = dir * kEnemySpeed;
                e.move.facingYaw = std::atan2(dir.x, dir.z);
                e.timer = 0.0f;  // подходя заново — первый удар не мгновенный
            } else {
                e.timer += dt;
                if (enemyStats_.attackInterval > 0.0f && e.timer >= enemyStats_.attackInterval) {
                    e.timer -= enemyStats_.attackInterval;
                    core->hp -= e.damage;  // урон по ядру
                }
            }
            if (world_ && e.move.collider != 0)
                e.move.position =
                    world_->moveCharacter(e.move.collider, vel, e.move.velocityY, false, dt);
            else
                e.move.position = e.move.position + vel * dt;
        }
    }

    // Башни: по кулдауну (rate) бьют ближайшего врага в радиусе (range). Урон правит hp
    // цели — структура entities_ не меняется, поэтому указатели/итерация безопасны.
    for (Entity& tw : entities_) {
        if (tw.type != EntityType::Tower) continue;
        tw.timer += dt;
        if (tw.rate <= 0.0f || tw.timer < tw.rate) continue;
        Entity* target = nullptr;
        float bestD2 = tw.range * tw.range;
        for (Entity& en : entities_) {
            if (en.type != EntityType::Enemy || en.hp <= 0.0f) continue;
            Vec3 d = en.move.position - tw.move.position;
            float d2 = d.x * d.x + d.y * d.y + d.z * d.z;
            if (d2 <= bestD2) { bestD2 = d2; target = &en; }
        }
        if (target != nullptr) {
            tw.timer -= tw.rate;
            target->hp -= tw.damage;
        } else {
            tw.timer = tw.rate;  // цели нет — держим заряд готовым (без роста таймера)
        }
    }

    // Уборка убитых врагов (hp<=0): удаляем сущность и её контроллер. Делаем ПОСЛЕ всех
    // систем — тут указатель core инвалидируется, дальше им не пользуемся.
    for (size_t i = 0; i < entities_.size();) {
        Entity& e = entities_[i];
        if (e.type == EntityType::Enemy && e.hp <= 0.0f) {
            if (world_ && e.move.collider != 0) world_->removeCharacter(e.move.collider);
            entities_.erase(entities_.begin() + (long)i);
        } else {
            ++i;
        }
    }

    // Жизненный цикл матча (свежие сканы — core-указатель мог инвалидироваться уборкой):
    // ядро разрушено → поражение; все спавнеры отработали и врагов нет → победа.
    float coreHpNow = 0.0f, coreMaxHp = 0.0f;
    bool haveCore = false;
    bool allSpawned = true, haveSpawner = false;
    int enemies = 0;
    for (const Entity& e : entities_) {
        if (e.type == EntityType::Core) { coreHpNow = e.hp; coreMaxHp = e.maxHp; haveCore = true; }
        else if (e.type == EntityType::Spawner) {
            haveSpawner = true;
            if (e.spawnedCount < (int)e.cap) allSpawned = false;
        } else if (e.type == EntityType::Enemy) {
            ++enemies;
        }
    }
    // maxHp>0 — иначе ядро без заданного здоровья (плейсхолдер) считалось бы разрушенным сразу.
    if (haveCore && coreMaxHp > 0.0f && coreHpNow <= 0.0f) {
        phase_ = GamePhase::Lost;
        LOGI("GameWorld: ядро разрушено — поражение");
    } else if (haveSpawner && allSpawned && enemies == 0) {
        phase_ = GamePhase::Won;
        LOGI("GameWorld: все волны отбиты — победа");
    }
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
    }
}

uint32_t GameWorld::inputSeq(uint32_t heroId) const {
    const Entity* h = entityById(heroId);
    return h != nullptr ? h->input.seq : 0;
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
