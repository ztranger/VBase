#pragma once

#include "game/GameTypes.h"  // EntityType

// P2-18: общие константы/предикаты боевой косметики. Нужны и ПРОДЮСЕРУ (ClientWorld::applySnapshot —
// диф урона порождает числа/искры/вспышку/тряску), и ПОТРЕБИТЕЛЮ (Scene::render — старение эффектов
// и HP-бары). Без состояния; чистые данные/функции.
inline constexpr float kFlashDur = 0.12f;     // длительность hit-flash / scale-punch, сек
inline constexpr float kDmgLife = 0.9f;       // время жизни всплывающего числа урона, сек
inline constexpr float kDmgMinAmount = 0.5f;  // порог урона, ниже — число/искры не показываем
inline constexpr float kSparkLife = 0.28f;    // время жизни искр в точке хита, сек
inline constexpr float kPunch = 0.12f;        // амплитуда scale-punch модели при уроне (доля)
inline constexpr float kShakeDur = 0.35f;     // длительность тряски камеры, сек
inline constexpr float kPoofLife = 0.5f;      // время жизни «пуфа» постройки, сек

// Есть ли у типа боевая полоска HP (враги/герои/башни/ядро). Здания-экономика (hp=0) — нет.
inline bool isCombatType(EntityType t) {
    return t == EntityType::Enemy || t == EntityType::Hero ||
           t == EntityType::Tower || t == EntityType::Core;
}

// Высота точки «над головой» для бара/числа (грубо под визуал типа).
inline float combatHeadHeight(EntityType t) {
    switch (t) {
        case EntityType::Tower: return 3.0f;
        case EntityType::Core:  return 2.6f;
        case EntityType::Hero:  return 2.4f;
        default:                return 2.1f;  // Enemy и пр.
    }
}
