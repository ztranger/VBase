#pragma once

#include <cmath>

#include "engine/core/MathUtil.h"

// Строительная сетка базы — ОБЩАЯ для сервера (валидация размещения в GameWorld) и клиента
// (снап призрака, проверка занятости/границ). Одна геометрия сетки на обе стороны — иначе
// клиент показывал бы валидной клетку, которую сервер отвергнет.
namespace grid {

constexpr float kCell = 2.0f;        // размер клетки, world-единиц
constexpr float kArenaHalf = 11.0f;  // строим в пределах |x|,|z| < этого (внутри стен ±12)

inline int cellOf(float w) { return (int)std::floor(w / kCell); }
inline float centerOf(int c) { return ((float)c + 0.5f) * kCell; }
inline Vec3 cellCenter(int cx, int cz) { return Vec3{centerOf(cx), 0.0f, centerOf(cz)}; }

inline bool inArena(int cx, int cz) {
    float x = centerOf(cx), z = centerOf(cz);
    return x > -kArenaHalf && x < kArenaHalf && z > -kArenaHalf && z < kArenaHalf;
}

}  // namespace grid
