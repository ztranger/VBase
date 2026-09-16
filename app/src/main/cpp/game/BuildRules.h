#pragma once

#include "engine/core/MathUtil.h"
#include "engine/net/Net.h"  // EntityType (сетевой контракт типов сущностей)

// Единые правила стройки/навигации — ОДИН источник для сервера (GameWorld) и клиента (Scene),
// чтобы предикаты не расходились молча (раньше были ручные копии с комментарием «должно
// совпадать с сервером»). Платформонезависимо: без рендера/платформы.

// Занимает ли тип клетку сетки (проверка коллизии размещения на сервере). Спавнер занимает.
inline bool isBuildingType(EntityType t) {
    return t == EntityType::Generator || t == EntityType::Storage ||
           t == EntityType::Spawner || t == EntityType::Tower || t == EntityType::Core;
}

// Блокирует ли тип путь мобов (футпринт + occupancy навсетки). Спавнер — НЕТ (из него
// выходят враги), враг/герой — тоже нет. Занимает клетку в поле потока и в предсказании героя.
inline bool blocksPath(EntityType t) {
    return t == EntityType::Generator || t == EntityType::Storage ||
           t == EntityType::Tower || t == EntityType::Core;
}

// Бокс футпринта здания по мировой позиции: клетка-в-размер, центр = позиция (НЕ cellCenter —
// здания из сцены часто не по сетке; иначе физбокс и occupancy разъехались бы на полклетки).
// ОДНА геометрия для физики (Jolt) и occupancy (навсетка) на обеих сторонах.
inline void footprintBox(const Vec3& pos, float cell, Vec3& center, Vec3& half) {
    const float h = cell * 0.5f;
    center = Vec3{pos.x, 0.5f, pos.z};
    half = Vec3{h, 0.5f, h};
}
