#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <unordered_map>
#include <vector>

#include "engine/core/Input.h"
#include "engine/core/MathUtil.h"
#include "engine/net/Net.h"  // EntityType, EntityState, GamePhase (словарь протокола)
#include "game/Character.h"
#include "game/SceneDesc.h"  // SceneDesc, EnemySpec (по значению)

class CollisionWorld;

// Авторитетная игровая сущность (сервер). Толстый tagged-union по типу: поля, не нужные
// данному типу, просто не используются. При сотнях сущностей это дёшево и кэш-дружелюбно —
// полноценный ECS здесь преждевременен. Движение/трансформ — в `move` (Character): для
// героя это тот же код, что предсказывает клиент; для зданий/врагов `move.position/facingYaw`
// служат трансформом, а поведение задаёт система по типу.
struct Entity {
    uint32_t id = 0;
    EntityType type = EntityType::Hero;
    uint8_t team = 0;
    Character move;       // трансформ + (для подвижных) физика/анимация
    InputCommand input;   // герой: последний ввод (иначе не используется)
    float hp = 0.0f, maxHp = 0.0f;
    float aux = 0.0f;     // сеть: ресурс в хранилище / прогресс / …
    // Серверные параметры/рантайм (не в сети — фиксированы сценой или считаются здесь).
    float rate = 0.0f;    // generator: ресурс/сек; spawner: интервал спавна; tower: интервал выстрела
    float cap = 0.0f;     // storage: ёмкость; spawner: макс. врагов
    float damage = 0.0f;  // tower: урон выстрела; enemy: урон по ядру за удар
    float range = 0.0f;   // tower: радиус поражения
    float timer = 0.0f;   // spawner: до след. спавна; tower: до след. выстрела; enemy: до след. удара
    int spawnedCount = 0; // spawner: сколько врагов уже породил (потолок = cap)
};

// Буфер ввода героя. Клиент шлёт по одному InputCommand за тик; сервер НЕ перезаписывает
// «последний ввод» (иначе пачка вводов за один poll коалесится — терялся прыжок и возникал
// rubber-band), а КОПИТ их в FIFO и потребляет по одному за тик (см. GameWorld::step).
// `lastSeq` (seq последнего потреблённого) — это ackSeq клиенту для реконсиляции.
struct HeroInputBuf {
    std::deque<InputCommand> queue;  // непотреблённые вводы (в порядке прихода)
    InputCommand last;               // последний потреблённый — «удерживаем» при пустой очереди
    uint32_t lastSeq = 0;            // seq последнего потреблённого (ackSeq)
    bool hasLast = false;
};

/**
 * Авторитетная игровая симуляция — БЕЗ сети. Владеет сущностями, экономикой и физическим
 * миром; прогоняет системы (движение героев, экономика, спавнеры, враги, дальше — бой).
 * NetServer — чистый транспорт: кормит мир вводом (`setHeroInput`), двигает его (`step`) и
 * сериализует состояние (`writeStates`). Благодаря отсутствию ENet весь геймплей
 * тестируется headless (см. server --selftest) и не тонет в сетевом коде.
 */
class GameWorld {
public:
    GameWorld();
    ~GameWorld();  // из-за unique_ptr<CollisionWorld> (неполный тип в заголовке)
    GameWorld(const GameWorld&) = delete;
    GameWorld& operator=(const GameWorld&) = delete;

    void reset();                             // снести всё (сущности, мир, ресурс)
    void configure(const SceneDesc& desc);    // построить мир коллизий + здания базы
    uint32_t addHero(uint8_t team = 0);       // создать героя (спавн + контроллер), вернуть id
    void removeEntity(uint32_t id);           // убрать сущность и её коллайдер
    void setHeroInput(uint32_t heroId, const InputCommand& in);
    // Попытка возвести здание героем на клетке сетки. Валидирует (тип buildable, хватает
    // ресурса, клетка в арене и свободна), тратит ресурс, спавнит сущность. false = отказ.
    bool tryBuild(uint32_t builderId, EntityType type, int cellX, int cellZ);
    void step(float dt);                      // прогнать все игровые системы на шаг dt
    void writeStates(std::vector<EntityState>& out) const;  // состояние всех сущностей -> сеть

    uint32_t inputSeq(uint32_t heroId) const; // seq последнего ввода героя (для ackSeq снапшота)
    GamePhase gamePhase() const { return phase_; }  // фаза матча (в заголовок снапшота)

    // Максимум команд (0 = соло/кооп; для PvP 2v2 хватает; запас на free-for-all).
    static constexpr int kMaxTeams = 4;

    // Отладка/самотесты.
    float resource(uint8_t team = 0) const {  // пул ресурса команды
        return team < kMaxTeams ? resourcePerTeam_[team] : 0.0f;
    }
    int enemyCount() const;
    float coreHp() const;  // здоровье первого ядра (для CombatTest)
    float heroHp(uint32_t id) const;   // здоровье героя по id (для HeroStakesTest); -1 если нет
    Vec3 heroPos(uint32_t id) const;   // позиция героя по id (для проверки респауна)

private:
    std::vector<Entity> entities_;            // ВСЕ сущности мира (герои + здания + враги)
    std::unordered_map<uint32_t, HeroInputBuf> heroInputs_;  // ввод по героям (heroId -> буфер)
    uint32_t nextEntityId_ = 1;
    std::unique_ptr<CollisionWorld> world_;   // та же геометрия, что у клиента (может быть пуст)
    Vec3 spawnPos_{0.0f, 0.0f, 0.0f};         // точка спавна героев (из сцены)
    float capsuleRadius_ = 0.3f;
    float capsuleCylHalf_ = 0.3f;
    float resourcePerTeam_[kMaxTeams] = {0.0f};  // пул ресурса на каждую команду
    EnemySpec enemyStats_;                    // hp/урон/интервал врага (из конфига через сцену)
    BuildTemplate buildTemplates_[8];         // шаблоны построек героя по EntityType (из сцены)
    float heroHp_ = 100.0f;                   // здоровье героя при спавне/респауне (из конфига)
    float heroRespawn_ = 5.0f;                // задержка респауна героя, сек (из конфига)
    Grid grid_;                               // строительная сетка (из описания сцены)
    GamePhase phase_ = GamePhase::Playing;    // жизненный цикл матча

    Entity* entityById(uint32_t id);
    const Entity* entityById(uint32_t id) const;
};
