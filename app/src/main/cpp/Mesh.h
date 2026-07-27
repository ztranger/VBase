#pragma once

#include <cstdint>
#include <vector>

// Вершина: позиция + нормаль (для освещения) + UV (для текстур).
struct Vertex {
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
};

// CPU-описание меша. Рендер заливает его в GPU через createMesh() и возвращает handle.
struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
};

// Непрозрачный идентификатор меша внутри рендера. 0 — невалидный.
using MeshHandle = uint32_t;

// Генераторы примитивов (backend-агностичные, чистая геометрия с UV).
MeshData makePlane(float size, float uvTiles = 1.0f);             // плоскость в XZ, нормаль вверх
MeshData makeCube(float size);                                    // куб с рёбрами size
MeshData makeSphere(float radius, int stacks = 16, int slices = 24); // UV-сфера
