#pragma once

#include <cstddef>
#include <cstdint>

// Игровые типы сетевого контракта: тег сущности, фаза матча, состояние сущности в снапшоте.
// P2-19: вынесены из ТРАНСПОРТНОГО engine/net/Net.h — игровые типы не должны жить в слое сети.
// Чистые данные без зависимостей; и транспорт (engine/net сериализует), и игровые слои
// (game/GameWorld, game/Scene) включают их отсюда. Раскладка EntityState — часть протокола
// (kProtocolVersion в Net.h), поэтому static_assert'ы едут вместе с ней.

// Тип игровой сущности (тег в снапшоте; по нему клиент выбирает визуал/поведение).
enum class EntityType : uint8_t {
    Hero = 0,
    Generator,
    Storage,
    Spawner,
    Enemy,
    Tower,
    Core,
    Projectile,  // снаряд башни (серверная сущность): летит к цели, урон по попаданию
    Trap,        // напольная ловушка (шипы): НЕ блокирует путь мобов, периодический AoE-урон
};

// Число значений EntityType (для массивов-по-типу и валидации сетевого тега).
constexpr int kEntityTypeCount = (int)EntityType::Trap + 1;

// Фаза матча (жизненный цикл). Глобальна (не per-entity) — шлётся в заголовке снапшота.
enum class GamePhase : uint8_t {
    Playing = 0,  // идёт бой
    Won = 1,      // все спавнеры отработали и врагов не осталось
    Lost = 2,     // ядро разрушено
};

// Состояние одной сущности в снапшоте (то, что сервер шлёт клиентам). Обобщено под систему
// сущностей: тип + команда + generic-слоты (hp / aux — ресурс/прогресс/…).
struct EntityState {
    uint32_t id = 0;
    uint8_t type = 0;      // EntityType
    uint8_t team = 0;      // 0 = нейтрал/PvE; 1/2 — стороны (кооп/PvP)
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float yaw = 0.0f;
    float animParam = 0.0f;
    float speed01 = 0.0f;
    float velY = 0.0f;     // вертикальная скорость (для реконсиляции прыжка на клиенте)
    float hp = 0.0f;       // здоровье (герой/враг/здание); 0 = не используется
    float aux = 0.0f;      // generic-слот по типу: ресурс в хранилище, прогресс, …
    float attackT = 0.0f;  // остаток времени атаки героя, сек (>0 = идёт каст; для анимации)
    uint8_t charType = 0;  // индекс персонажа в ростере (какой моделью рисовать героя)
};

// EntityState шлётся сырым memcpy (Net.cpp) и НЕ входит в pack(1)-блок — между team (off 5)
// и x (off 8) есть 2 байта паддинга. На ARM64/x64 (обе LE, natural-align) раскладка совпадает,
// но kProtocolVersion этого не ловит: страхуемся статик-проверкой. Любое поле, сместившее
// layout, уронит сборку ЗДЕСЬ (а не породит порчу памяти в снапшотах) — напоминание бампнуть
// версию И сверить раскладку на обеих целях.
static_assert(sizeof(EntityState) == 52, "EntityState: размер изменился — бампни kProtocolVersion");
static_assert(alignof(EntityState) == 4, "EntityState: выравнивание изменилось");
static_assert(offsetof(EntityState, x) == 8, "EntityState: паддинг после team съехал");
