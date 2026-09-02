#include "game/GameWorld.h"

#include <cmath>
#include <memory>
#include <vector>

#include "engine/core/Log.h"
#include "engine/physics/CollisionWorld.h"
#include "game/FlowField.h"
#include "game/Grid.h"
#include "game/SceneDesc.h"

namespace {
// Параметры врагов: скорость бега, капсула, дистанция «дошёл до цели».
constexpr float kEnemySpeed = 3.0f;
constexpr float kEnemyRadius = 0.3f;
constexpr float kEnemyCylHalf = 0.3f;
constexpr float kBuildingHitRange = 2.6f;  // мили по зданию (полуклетка + капсула + угол)
constexpr float kHeroHitRange = 1.3f;      // враг бьёт героя, если тот ближе этого

float horizDist(Vec3 a, Vec3 b) {
    float dx = a.x - b.x, dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

// Снаряд башни (серверная сущность): летит к цели, урон по попаданию.
constexpr float kProjSpeed = 16.0f;      // world/сек
constexpr float kProjHitRadius = 1.0f;   // радиус попадания (>= kProjSpeed*dt, чтобы не проскочить)
constexpr float kProjMaxLife = 2.5f;     // сек до самоуничтожения (если цель пропала)
constexpr float kTowerMuzzleY = 2.2f;    // высота вылета снаряда над башней
constexpr float kHeroMuzzleY = 1.4f;     // высота вылета снаряда героя (грудь/посох)
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

// Блокирует ли тип путь мобов (футпринт + occupancy). Спавнер — нет: из него выходят.
bool blocksPath(EntityType t) {
    return t == EntityType::Generator || t == EntityType::Storage ||
           t == EntityType::Tower || t == EntityType::Core;
}

// Бокс футпринта здания — ОДНА геометрия для физики (Jolt) и occupancy (навсетка):
// поле потока не должно вести мобов в клетку, где стоит коллайдер. Клетка-в-размер,
// центр = позиция здания (НЕ cellCenter: здания из сцены часто не по сетке — иначе
// физбокс и занятость разъехались бы на полклетки).
void footprintBox(const Entity& e, float cell, Vec3& center, Vec3& half) {
    const float h = cell * 0.5f;
    center = Vec3{e.move.position.x, 0.5f, e.move.position.z};
    half = Vec3{h, 0.5f, h};
}

// Враждебны ли команды (кого бой считает целью). team 0 — нейтрал/PvE: враждебен всем,
// в т.ч. сам себе — иначе чисто-PvE-сцена (всё team 0) перестала бы воевать. Стороны 1/2
// враждебны разным командам и дружественны своей (PvP: башни/враги не бьют своих).
bool hostile(uint8_t a, uint8_t b) {
    if (a == 0 && b == 0) return true;  // чистый PvE — прежнее поведение
    return a != b;
}
}  // namespace

struct NavState {
    NavGrid map;
    FlowField toCore[GameWorld::kMaxTeams];
    FlowField toBuildings[GameWorld::kMaxTeams];
    bool mapDirty = true;                             // occupancy устарела (постройка/снос)
    bool fieldValid[GameWorld::kMaxTeams] = {false};  // поле команды актуально для текущей occupancy
};

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
    sceneColliders_.clear();
    nav_.reset();
    matchRestartDelay_ = 0.0f;
    restartTimer_ = 0.0f;
}

void GameWorld::configure(const SceneDesc& desc) {
    reset();  // идемпотентно: свежий мир на каждую конфигурацию
    world_ = std::make_unique<CollisionWorld>();
    sceneColliders_ = desc.colliders;
    for (const ColliderSpec& cs : desc.colliders) {
        world_->addBox(cs.center, cs.half);
    }
    // finalize() перенесён на ПОСЛЕ spawnBuildings — broad-phase оптимизируется со
    // статикой сцены И футпринтами базовых зданий сразу (см. ниже).
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
    world_->finalize();  // broad-phase со статикой сцены И футпринтами базовых зданий
    nav_ = std::make_unique<NavState>();  // occupancy пересчитаем на первом step
    rebuildNavIfNeeded();
    LOGI("GameWorld: мир — %d коллайдеров, %d сущностей базы, навсетка %dx%d",
         (int)desc.colliders.size(), (int)desc.buildings.size(),
         nav_->map.width(), nav_->map.height());
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
        e.waveSize = b.waveSize;   // spawner: бесконечные волны (0 = легаси по cap)
        e.wavePause = b.wavePause;
        e.waveGrow = b.waveGrow;
        entities_.push_back(e);
        attachFootprint(entities_.back());
    }
}

void GameWorld::restartMatch() {
    // Пересобрать матч, НЕ трогая геометрию/конфиг/подключения: убрать здания и врагов, заново
    // заспавнить базы, воскресить героев в их точках, обнулить экономику и исход. Игроки остаются.
    for (size_t i = 0; i < entities_.size();) {
        Entity& e = entities_[i];
        if (e.type == EntityType::Hero) { ++i; continue; }  // герои остаются (игроки подключены)
        detachPhysics(e);
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
    if (e != nullptr) detachPhysics(*e);
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
    e.damage = 0.0f; e.range = 0.0f; e.rate = 0.0f;  // авто-атака выключена без конфига
    if (e.charType < heroTypes_.size()) {
        const CharacterDesc& t = heroTypes_[e.charType];
        if (t.hp > 0.0f) hp = t.hp;
        if (t.speed > 0.0f) spd = t.speed;
        e.damage = t.damage;          // урон авто-атаки
        e.range = t.range;            // дальность (0 -> не атакует)
        e.rate = t.attackInterval;    // кулдаун между атаками (e.timer копит)
    }
    e.maxHp = hp;
    e.hp = hp;  // полный hp при выборе персонажа
    if (spd > 0.0f) e.move.maxSpeed = spd;
}

bool GameWorld::hasLineOfSight(Vec3 a, Vec3 b) const {
    if (nav_ == nullptr) return true;  // без навсетки — считаем видимым
    const NavGrid& m = nav_->map;
    int ax = grid_.cellOf(a.x), az = grid_.cellOf(a.z);
    int bx = grid_.cellOf(b.x), bz = grid_.cellOf(b.z);
    if (ax == bx && az == bz) return true;
    // Bresenham по клеткам: заблокированная ПРОМЕЖУТОЧНАЯ клетка = нет линии видимости.
    // Концы (клетки героя и врага) не проверяем — они могут быть у стены/на кромке футпринта.
    int dx = bx > ax ? bx - ax : ax - bx;
    int dz = bz > az ? bz - az : az - bz;
    int sx = ax < bx ? 1 : -1, sz = az < bz ? 1 : -1;
    int err = dx - dz;
    int cx = ax, cz = az;
    for (int guard = 0; guard <= 2 * (dx + dz) + 2; ++guard) {
        int e2 = 2 * err;
        if (e2 > -dz) { err -= dz; cx += sx; }
        if (e2 < dx) { err += dx; cz += sz; }
        if (cx == bx && cz == bz) break;  // дошли до клетки цели — не проверяем
        if (m.inBounds(cx, cz) && m.isBlocked(cx, cz)) return false;
    }
    return true;
}

void GameWorld::attachFootprint(Entity& e) {
    if (!blocksPath(e.type)) return;
    if (world_) {
        Vec3 c, h;
        footprintBox(e, grid_.cell, c, h);
        e.footprint = world_->addBox(c, h);
    }
    if (nav_) nav_->mapDirty = true;
}

void GameWorld::detachPhysics(Entity& e) {
    if (world_ == nullptr) return;
    if (e.move.collider != 0) {
        world_->removeCharacter(e.move.collider);
        e.move.collider = 0;
    }
    if (e.footprint != 0) {
        world_->removeBox(e.footprint);
        e.footprint = 0;
        if (nav_) nav_->mapDirty = true;
    }
}

void GameWorld::rebuildNavIfNeeded() {
    if (nav_ == nullptr || !nav_->mapDirty) return;
    nav_->mapDirty = false;
    nav_->map.reset(grid_);
    for (const ColliderSpec& cs : sceneColliders_) {
        nav_->map.rasterizeBox(cs.center, cs.half, kEnemyRadius);
    }
    // Футпринты зданий — той же геометрией, что физбокс (footprintBox), и растеризацией
    // (а не одной клеткой): здание не по сетке перекрывает до 4 клеток, иначе поле провело
    // бы моба в полузанятую клетку, где капсула упрётся в коллайдер. Без clearance —
    // соседние клетки застройки игрока должны оставаться проходимыми.
    for (const Entity& e : entities_) {
        if (!blocksPath(e.type)) continue;
        Vec3 c, h;
        footprintBox(e, grid_.cell, c, h);
        nav_->map.rasterizeBox(c, h);
    }
    // Поля потока пересчитываем ЛЕНИВО (ensureFlowField) — только для команд, у которых
    // реально есть мобы. Здесь лишь помечаем их устаревшими под новую occupancy: в PvE
    // это 2 BFS (team 0) вместо 2*kMaxTeams на каждое изменение построек.
    for (int t = 0; t < kMaxTeams; ++t) nav_->fieldValid[t] = false;
}

void GameWorld::ensureFlowField(uint8_t team) {
    if (nav_ == nullptr || team >= (uint8_t)kMaxTeams || nav_->fieldValid[team]) return;
    nav_->fieldValid[team] = true;
    // Цели команды team — ВРАЖДЕБНЫЕ ей живые постройки (ядра отдельно — для рашеров).
    // Цели зависят только от построек (не от мобов), а те всегда помечают mapDirty →
    // поле, посчитанное здесь при первом мобе, актуально до следующего изменения построек.
    std::vector<NavCell> cores, buildings;
    for (const Entity& e : entities_) {
        if (!blocksPath(e.type) || !hostile(team, e.team)) continue;
        if (e.maxHp > 0.0f && e.hp <= 0.0f) continue;  // мёртвое ядро/здание — не цель
        NavCell c{grid_.cellOf(e.move.position.x), grid_.cellOf(e.move.position.z)};
        if (e.type == EntityType::Core) cores.push_back(c);
        if (e.maxHp > 0.0f) buildings.push_back(c);
    }
    nav_->toCore[team].compute(nav_->map, cores);
    nav_->toBuildings[team].compute(nav_->map, buildings);
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
    entities_.push_back(e);
    attachFootprint(entities_.back());
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

    // Спавнеры: плодят врагов с боевыми статами из конфига. Два режима на спавнер:
    //  - бесконечные волны (waveSize>0): волна из curWaveSize врагов через `rate`, затем
    //    пауза `wavePause`, затем следующая волна (размер растёт на waveGrow). Не иссякают.
    //  - легаси (waveSize==0): спавн до потолка `cap`, затем стоп (как было).
    // Новые сущности КОПИМ и добавляем ПОСЛЕ цикла — push_back инвалидировал бы итерацию.
    {
        std::vector<Entity> spawned;
        // Один враг из спавнера e -> в `spawned` (тип/статы по charType, коллайдер).
        auto spawnEnemy = [&](Entity& e) {
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
            enemy.mobGoal = 0;
            if (nTypes > 0) enemy.mobGoal = (uint8_t)enemyTypes_[type].goal;
            if (world_)
                enemy.move.collider =
                    world_->addCharacter(e.move.position, kEnemyRadius, kEnemyCylHalf);
            spawned.push_back(enemy);
            e.spawnedCount++;
        };

        for (Entity& e : entities_) {
            if (e.type != EntityType::Spawner || e.rate <= 0.0f) continue;
            if (e.waveSize > 0) {
                // --- Бесконечные волны ---
                int curWave = e.waveSize + e.waveIndex * e.waveGrow;
                if (curWave < 1) curWave = 1;
                if (e.spawnedInWave >= curWave) {
                    // Пауза между волнами: копим timer до wavePause, затем следующая волна.
                    e.timer += dt;
                    if (e.timer >= e.wavePause) {
                        e.timer = 0.0f;
                        ++e.waveIndex;
                        e.spawnedInWave = 0;
                    }
                    continue;
                }
                e.timer += dt;
                if (e.timer >= e.rate) {
                    e.timer -= e.rate;
                    spawnEnemy(e);
                    ++e.spawnedInWave;
                }
            } else {
                // --- Легаси: спавн до потолка cap ---
                if (e.spawnedCount >= (int)e.cap) continue;
                e.timer += dt;
                if (e.timer >= e.rate) {
                    e.timer -= e.rate;
                    spawnEnemy(e);
                }
            }
        }
        for (Entity& n : spawned) entities_.push_back(std::move(n));
    }

    // Типизированные списки одним O(N)-проходом — чтобы боевые системы НЕ сканировали весь
    // вектор на каждой сущности (было O(N²): каждый враг искал ядра/героев по всем entities_,
    // каждая башня — врагов). entities_ стабилен от спавна выше до уборки трупов ниже —
    // указатели живут. Ядер/героев/башен немного, врагов может быть много: теперь бой ~O(N).
    std::vector<Entity*> cores, heroes, enemies, smashables;
    for (Entity& e : entities_) {
        if (e.type == EntityType::Core) cores.push_back(&e);
        else if (e.type == EntityType::Hero) heroes.push_back(&e);
        else if (e.type == EntityType::Enemy) enemies.push_back(&e);
        if (blocksPath(e.type) && e.maxHp > 0.0f && e.hp > 0.0f) smashables.push_back(&e);
    }

    rebuildNavIfNeeded();

    // Враги: поле потока к предпочитаемой цели (ядро / ближайшая постройка). Если рашер
    // не может дойти до ядра — фолбэк на поле построек и ломает то, до чего дошёл.
    for (Entity* ep : enemies) {
        Entity& e = *ep;
        const int cx = grid_.cellOf(e.move.position.x);
        const int cz = grid_.cellOf(e.move.position.z);
        const uint8_t team = (e.team < kMaxTeams) ? e.team : 0;

        bool smashBuildings = false;
        Vec3 dir{0.0f, 0.0f, 0.0f};
        if (nav_ != nullptr && nav_->map.cellCount() > 0) {
            ensureFlowField(team);  // ленивый пересчёт поля этой команды (раз на dirty)
            const bool preferCore =
                (e.mobGoal != (uint8_t)CharacterDesc::MobGoal::Building);
            if (preferCore && nav_->toCore[team].reachable(cx, cz)) {
                dir = nav_->toCore[team].direction(cx, cz);
            } else if (nav_->toBuildings[team].reachable(cx, cz)) {
                dir = nav_->toBuildings[team].direction(cx, cz);
                smashBuildings = true;
            } else if (nav_->toCore[team].reachable(cx, cz)) {
                dir = nav_->toCore[team].direction(cx, cz);
            }
        }

        Entity* core = nullptr;
        float coreDist = 1e30f;
        for (Entity* cp : cores) {
            if (!hostile(e.team, cp->team)) continue;
            float d = horizDist(e.move.position, cp->move.position);
            if (d < coreDist) { coreDist = d; core = cp; }
        }
        // Нет навсетки — как раньше: прямая на ядро.
        if (nav_ == nullptr || nav_->map.cellCount() == 0) {
            smashBuildings = false;
            if (core != nullptr && coreDist > kBuildingHitRange) {
                Vec3 to = core->move.position - e.move.position;
                to.y = 0.0f;
                dir = to * (1.0f / coreDist);
            } else {
                dir = Vec3{0.0f, 0.0f, 0.0f};
            }
        }

        Entity* atk = nullptr;
        float atkDist = 1e30f;
        if (smashBuildings) {
            // Ломатель / фолбэк: идём по полю потока (обход статики). Прямая на
            // здание — только в мили: иначе капсула лезет сквозь неломаемые кубы.
            Entity* dest = nullptr;
            float destDist = 1e30f;
            Entity* sticky = entityById(e.targetId);
            auto okB = [&](Entity* b) {
                return b != nullptr && b->hp > 0.0f && b->maxHp > 0.0f &&
                       blocksPath(b->type) && hostile(e.team, b->team);
            };
            if (okB(sticky)) {
                destDist = horizDist(e.move.position, sticky->move.position);
                if (destDist <= kBuildingHitRange) dest = sticky;
            }
            if (dest == nullptr) {
                e.targetId = 0;
                destDist = 1e30f;
                for (Entity* b : smashables) {
                    if (!okB(b)) continue;
                    float d = horizDist(e.move.position, b->move.position);
                    if (d <= kBuildingHitRange && d < destDist) {
                        dest = b;
                        destDist = d;
                    }
                }
            }
            if (dest != nullptr) {
                e.targetId = dest->id;
                atk = dest;
                atkDist = destDist;
                dir = Vec3{0.0f, 0.0f, 0.0f};
            }
        } else {
            e.targetId = 0;
            if (core != nullptr && coreDist <= kBuildingHitRange) {
                atk = core;
                atkDist = coreDist;
            }
        }
        for (Entity* hero : heroes) {
            if (hero->hp <= 0.0f || !hostile(e.team, hero->team)) continue;
            float hd = horizDist(e.move.position, hero->move.position);
            if (hd <= kHeroHitRange && hd < atkDist) { atk = hero; atkDist = hd; }
        }

        Vec3 vel{0.0f, 0.0f, 0.0f};
        if (atk != nullptr && atk->type != EntityType::Hero) {
            Vec3 to = atk->move.position - e.move.position;
            to.y = 0.0f;
            float len = std::sqrt(to.x * to.x + to.z * to.z);
            if (len > 1e-4f) e.move.facingYaw = std::atan2(to.x / len, to.z / len);
        } else if (dir.x != 0.0f || dir.z != 0.0f) {
            float spd = e.move.maxSpeed > 0.0f ? e.move.maxSpeed : kEnemySpeed;
            vel = dir * spd;
            e.move.facingYaw = std::atan2(dir.x, dir.z);
        }

        if (atk != nullptr) {
            e.timer += dt;
            float atkInt = e.rate > 0.0f ? e.rate : enemyStats_.attackInterval;
            if (atkInt > 0.0f && e.timer >= atkInt) {
                e.timer -= atkInt;
                atk->hp -= e.damage;
            }
        } else {
            e.timer = 0.0f;
        }
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
    // Герой: авто-атака ближайшего врага в range при линии видимости. melee -> мгновенный
    // урон, ranged (маг) -> снаряд (как башня). Атака НЕ рутит движение (см. Character::simulate),
    // поэтому чисто аддитивна и не конфликтует с предсказанием. Кулдаун копит e.timer.
    for (Entity* h : heroes) {
        if (h->hp <= 0.0f) { h->timer = 0.0f; continue; }
        if (h->range <= 0.0f || h->damage <= 0.0f) continue;  // авто-атака не настроена
        Entity* target = nullptr;
        float bestD2 = h->range * h->range;
        for (Entity* enp : enemies) {  // мобы
            if (enp->hp <= 0.0f || !hostile(h->team, enp->team)) continue;
            Vec3 d = enp->move.position - h->move.position;
            float d2 = d.x * d.x + d.z * d.z;  // горизонтальная дистанция
            if (d2 <= bestD2 && hasLineOfSight(h->move.position, enp->move.position)) {
                bestD2 = d2; target = enp;
            }
        }
        for (Entity* oh : heroes) {  // чужие герои (PvP)
            if (oh == h || oh->hp <= 0.0f || !hostile(h->team, oh->team)) continue;
            Vec3 d = oh->move.position - h->move.position;
            float d2 = d.x * d.x + d.z * d.z;
            if (d2 <= bestD2 && hasLineOfSight(h->move.position, oh->move.position)) {
                bestD2 = d2; target = oh;
            }
        }
        float atkInt = h->rate > 0.0f ? h->rate : 1.0f;
        h->timer += dt;
        if (h->timer > atkInt) h->timer = atkInt;  // заряд готов (не копим сверх)
        if (target == nullptr || h->timer < atkInt) continue;
        h->timer = 0.0f;
        Vec3 to = target->move.position - h->move.position;
        float len = std::sqrt(to.x * to.x + to.z * to.z);
        if (len > 1e-4f) h->move.facingYaw = std::atan2(to.x / len, to.z / len);
        h->move.attackTime = Character::kAttackDuration;  // клип атаки (едет клиенту в снапшоте)
        bool ranged = (h->charType < heroTypes_.size()) && heroTypes_[h->charType].ranged;
        if (ranged) {
            Entity proj;
            proj.id = nextEntityId_++;
            proj.type = EntityType::Projectile;
            proj.team = h->team;
            proj.move.position = h->move.position + Vec3{0.0f, kHeroMuzzleY, 0.0f};
            proj.move.snapshot();
            proj.damage = h->damage;
            proj.move.maxSpeed = kProjSpeed;
            proj.targetId = target->id;  // хоминг по id цели
            Vec3 aim = (target->move.position + Vec3{0.0f, kEnemyAimY, 0.0f}) - proj.move.position;
            proj.move.facingYaw = std::atan2(aim.x, aim.z);
            projSpawned.push_back(std::move(proj));
        } else {
            target->hp -= h->damage;  // ближний бой — мгновенный урон
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
        // Хомит по врагу (моб) ИЛИ чужому герою (PvP) — цель по id, тип проверяем для наведения.
        if (tgt != nullptr && (tgt->type == EntityType::Enemy || tgt->type == EntityType::Hero) &&
            tgt->hp > 0.0f) {
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
        bool deadBuilding = blocksPath(e.type) && e.type != EntityType::Core &&
                            e.maxHp > 0.0f && e.hp <= 0.0f;
        if (deadEnemy || deadProj || deadBuilding) {
            detachPhysics(e);
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
            // Бесконечный спавнер (waveSize>0) никогда не «отработал» → PvE становится
            // выживанием (победы по волнам нет; проигрыш = ядро пало). Легаси — по cap.
            if (e.waveSize > 0 || e.spawnedCount < (int)e.cap) allSpawned = false;
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
