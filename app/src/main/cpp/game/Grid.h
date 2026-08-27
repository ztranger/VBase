#pragma once

#include <cmath>

#include "engine/core/MathUtil.h"

// Строительная сетка базы — параметры (размер клетки, размер зоны) приходят из ФАЙЛА СЦЕНЫ
// (директива `grid`), поэтому это значение-объект, а не константы. Сервер (GameWorld) и клиент
// (Scene) держат по копии из описания сцены — геометрия сетки ОДНА на обе стороны, иначе клиент
// показал бы валидной клетку, которую сервер отвергнет.
struct Grid {
    float cell = 2.0f;        // размер клетки, world-единиц
    float arenaHalf = 11.0f;  // строим в пределах |x|,|z| < этого (внутри стен ±12)

    int cellOf(float w) const { return (int)std::floor(w / cell); }
    float centerOf(int c) const { return ((float)c + 0.5f) * cell; }
    Vec3 cellCenter(int cx, int cz) const { return Vec3{centerOf(cx), 0.0f, centerOf(cz)}; }

    bool inArena(int cx, int cz) const {
        float x = centerOf(cx), z = centerOf(cz);
        return x > -arenaHalf && x < arenaHalf && z > -arenaHalf && z < arenaHalf;
    }

    // Целочисленный диапазон клеток, у которых центр внутри арены (включительно).
    // Закрытая формула — без перебора; на полях 200–400 клеток на сторону это важно.
    // Пустой диапазон: lo > hi (арена меньше одной клетки).
    void cellRange(int& lo, int& hi) const {
        const float inv = 1.0f / cell;
        lo = (int)std::floor(-arenaHalf * inv - 0.5f + 1e-4f) + 1;
        hi = (int)std::floor(arenaHalf * inv - 0.5f - 1e-5f);
    }
};
