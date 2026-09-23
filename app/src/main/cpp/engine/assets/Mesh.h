#pragma once

#include <cstdint>
#include <vector>

// Вершина: позиция + нормаль (освещение) + UV (текстуры) + тангент (normal mapping).
// Тангент — направление роста U в мировой геометрии; бинормаль шейдер берёт как
// cross(N, T). Для материалов без нормал-карты тангент не влияет (плоская нормаль).
struct Vertex {
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
    float tx, ty, tz;
};

// CPU-описание меша. Рендер заливает его в GPU через createMesh() и возвращает handle.
struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
};

// Непрозрачный идентификатор меша внутри рендера. 0 — невалидный.
using MeshHandle = uint32_t;

// Генераторы примитивов (backend-агностичные, чистая геометрия с UV).
MeshData makePlane(float size, float uvTiles = 1.0f);             // квадратная плоскость в XZ, нормаль вверх
MeshData makePlaneRect(float sizeX, float sizeZ,                  // прямоугольная плоскость в XZ (дорожки/полосы):
                       float uvX = 1.0f, float uvZ = 1.0f);       // независимые размеры и тайлинг UV по осям
// Подразбитая земля с лёгким рельефом: сетка cells×cells на size, высота — детерминированная
// аналитическая (сумма синусов, freq/amp), нормали и тангенты из неё. uvTiles — тайлинг текстуры.
// Радиальная маска (flatR/hillR): в радиусе flatR от центра рельеф = 0 (плоская игровая арена),
// к hillR плавно нарастает (холмы в окружении) — юниты/дорожка в арене не «плавают». hillR<=flatR
// -> без маски (равномерный рельеф). ВАЖНО: формула высоты повторена в gen_forest_demo.py (посадка
// декора на рельеф) — менять синхронно. Физика/пасфайндинг остаются плоскими (визуальный рельеф).
MeshData makeTerrain(float size, int cells, float amp, float freq, float uvTiles = 1.0f,
                     float flatR = 0.0f, float hillR = 0.0f);
float terrainHeight(float x, float z, float amp, float freq, float flatR = 0.0f, float hillR = 0.0f);
MeshData makeCube(float size);                                    // куб с рёбрами size
MeshData makeSphere(float radius, int stacks = 16, int slices = 24); // UV-сфера
