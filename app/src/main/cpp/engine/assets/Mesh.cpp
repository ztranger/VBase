#include "engine/assets/Mesh.h"

#include <cmath>

MeshData makePlane(float size, float uvTiles) {
    float h = size * 0.5f;
    float t = uvTiles;
    MeshData mesh;
    // Четыре угла на y=0, нормаль вверх, тангент вдоль +X (рост U). UV тайлятся.
    mesh.vertices = {
        {-h, 0.0f, -h, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f},
        { h, 0.0f, -h, 0.0f, 1.0f, 0.0f, t,    0.0f, 1.0f, 0.0f, 0.0f},
        { h, 0.0f,  h, 0.0f, 1.0f, 0.0f, t,    t,    1.0f, 0.0f, 0.0f},
        {-h, 0.0f,  h, 0.0f, 1.0f, 0.0f, 0.0f, t,    1.0f, 0.0f, 0.0f},
    };
    mesh.indices = {0, 2, 1, 0, 3, 2};
    return mesh;
}

MeshData makePlaneRect(float sizeX, float sizeZ, float uvX, float uvZ) {
    float hx = sizeX * 0.5f, hz = sizeZ * 0.5f;
    MeshData mesh;
    // Углы на y=0, нормаль вверх, тангент вдоль +X (рост U). UV тайлятся независимо по осям.
    mesh.vertices = {
        {-hx, 0.0f, -hz, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f},
        { hx, 0.0f, -hz, 0.0f, 1.0f, 0.0f, uvX,  0.0f, 1.0f, 0.0f, 0.0f},
        { hx, 0.0f,  hz, 0.0f, 1.0f, 0.0f, uvX,  uvZ,  1.0f, 0.0f, 0.0f},
        {-hx, 0.0f,  hz, 0.0f, 1.0f, 0.0f, 0.0f, uvZ,  1.0f, 0.0f, 0.0f},
    };
    mesh.indices = {0, 2, 1, 0, 3, 2};
    return mesh;
}

// Детерминированная высота рельефа: сумма синусов (в [-amp,amp]) с радиальной маской (плоская
// арена в центре). Повторена в gen_forest_demo.py (посадка декора) — держать синхронно.
float terrainHeight(float x, float z, float amp, float freq, float flatR, float hillR) {
    float f = freq;
    float h = 0.55f * std::sin(x * f) * std::cos(z * f * 0.9f) +
              0.30f * std::sin(x * f * 2.1f + 1.7f) * std::cos(z * f * 1.9f - 0.6f) +
              0.15f * std::sin((x + z) * f * 3.3f + 0.3f);
    if (hillR > flatR) {  // маска: 0 внутри flatR, плавно до 1 к hillR (smoothstep)
        float r = std::sqrt(x * x + z * z);
        float t = (r - flatR) / (hillR - flatR);
        if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
        h *= t * t * (3.0f - 2.0f * t);
    }
    return amp * h;
}

MeshData makeTerrain(float size, int cells, float amp, float freq, float uvTiles,
                     float flatR, float hillR) {
    if (cells < 1) cells = 1;
    if (cells > 512) cells = 512;  // потолок против взрыва вершин (недоверенный параметр сцены)
    const float half = size * 0.5f;
    const float step = size / (float)cells;
    const float e = step * 0.5f;  // шаг для центральной разности нормали/тангента
    MeshData mesh;
    mesh.vertices.reserve((size_t)(cells + 1) * (cells + 1));
    for (int j = 0; j <= cells; ++j) {
        for (int i = 0; i <= cells; ++i) {
            float x = -half + (float)i * step;
            float z = -half + (float)j * step;
            float y = terrainHeight(x, z, amp, freq, flatR, hillR);
            // Наклоны по X/Z центральной разностью -> нормаль (up-ish) и тангент вдоль +X.
            float dhx = (terrainHeight(x + e, z, amp, freq, flatR, hillR) -
                         terrainHeight(x - e, z, amp, freq, flatR, hillR)) / (2.0f * e);
            float dhz = (terrainHeight(x, z + e, amp, freq, flatR, hillR) -
                         terrainHeight(x, z - e, amp, freq, flatR, hillR)) / (2.0f * e);
            float nx = -dhx, ny = 1.0f, nz = -dhz;
            float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
            nx /= nl; ny /= nl; nz /= nl;
            float tx = 1.0f, ty = dhx, tz = 0.0f;  // dP/dx
            float tl = std::sqrt(tx * tx + ty * ty + tz * tz);
            tx /= tl; ty /= tl; tz /= tl;
            float u = (float)i / (float)cells * uvTiles;
            float v = (float)j / (float)cells * uvTiles;
            mesh.vertices.push_back({x, y, z, nx, ny, nz, u, v, tx, ty, tz});
        }
    }
    const int stride = cells + 1;
    mesh.indices.reserve((size_t)cells * cells * 6);
    for (int j = 0; j < cells; ++j) {
        for (int i = 0; i < cells; ++i) {
            uint32_t a = (uint32_t)(j * stride + i);
            uint32_t b = a + 1;
            uint32_t c = a + stride;
            uint32_t d = c + 1;
            mesh.indices.insert(mesh.indices.end(), {a, d, b, a, c, d});  // CCW сверху, как makePlane
        }
    }
    return mesh;
}

MeshData makeCube(float size) {
    float h = size * 0.5f;
    MeshData mesh;

    // По 4 вершины на грань — чтобы у каждой грани была своя нормаль.
    // Данные граней: нормаль + 4 угла (против часовой при взгляде снаружи).
    const float n[6][3] = {
        { 0,  0,  1}, { 0,  0, -1},
        { 1,  0,  0}, {-1,  0,  0},
        { 0,  1,  0}, { 0, -1,  0},
    };
    const float corners[6][4][3] = {
        {{-h,-h, h},{ h,-h, h},{ h, h, h},{-h, h, h}}, // +Z
        {{ h,-h,-h},{-h,-h,-h},{-h, h,-h},{ h, h,-h}}, // -Z
        {{ h,-h, h},{ h,-h,-h},{ h, h,-h},{ h, h, h}}, // +X
        {{-h,-h,-h},{-h,-h, h},{-h, h, h},{-h, h,-h}}, // -X
        {{-h, h, h},{ h, h, h},{ h, h,-h},{-h, h,-h}}, // +Y
        {{-h,-h,-h},{ h,-h,-h},{ h,-h, h},{-h,-h, h}}, // -Y
    };
    const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};

    for (int f = 0; f < 6; ++f) {
        // Тангент грани = направление роста U = ребро corner[1]-corner[0] (U идёт 0->1).
        float tx = corners[f][1][0] - corners[f][0][0];
        float ty = corners[f][1][1] - corners[f][0][1];
        float tz = corners[f][1][2] - corners[f][0][2];
        float tl = std::sqrt(tx * tx + ty * ty + tz * tz);
        if (tl > 0.0f) { tx /= tl; ty /= tl; tz /= tl; }
        uint32_t base = (uint32_t)mesh.vertices.size();
        for (int v = 0; v < 4; ++v) {
            mesh.vertices.push_back({
                corners[f][v][0], corners[f][v][1], corners[f][v][2],
                n[f][0], n[f][1], n[f][2],
                uv[v][0], uv[v][1],
                tx, ty, tz});
        }
        mesh.indices.push_back(base + 0);
        mesh.indices.push_back(base + 1);
        mesh.indices.push_back(base + 2);
        mesh.indices.push_back(base + 0);
        mesh.indices.push_back(base + 2);
        mesh.indices.push_back(base + 3);
    }
    return mesh;
}

MeshData makeSphere(float radius, int stacks, int slices) {
    // P2-12: недоверенные stacks/slices/radius из сцены/конфига. stacks/slices ниже 1 -> деление
    // на ноль (NaN-вершины); огромные -> раздутые буферы; radius NaN/inf/≤0 -> мусор. Клампим.
    if (stacks < 2) stacks = 2;
    if (slices < 3) slices = 3;
    if (stacks > 512) stacks = 512;
    if (slices > 512) slices = 512;
    if (!(radius > 0.0f) || !std::isfinite(radius)) radius = 1.0f;
    MeshData mesh;
    const float pi = 3.14159265358979323846f;

    for (int i = 0; i <= stacks; ++i) {
        float phi = pi * (float)i / (float)stacks;        // 0..pi (полюс к полюсу)
        float sinPhi = std::sin(phi), cosPhi = std::cos(phi);
        for (int j = 0; j <= slices; ++j) {
            float theta = 2.0f * pi * (float)j / (float)slices;
            float sinTheta = std::sin(theta), cosTheta = std::cos(theta);
            float nx = sinPhi * cosTheta;
            float ny = cosPhi;
            float nz = sinPhi * sinTheta;
            float uu = (float)j / (float)slices;
            float vv = (float)i / (float)stacks;
            // Тангент = d(pos)/d(theta), нормированный: (-sinTheta, 0, cosTheta).
            mesh.vertices.push_back({nx * radius, ny * radius, nz * radius,
                                     nx, ny, nz, uu, vv,
                                     -sinTheta, 0.0f, cosTheta});
        }
    }

    int stride = slices + 1;
    for (int i = 0; i < stacks; ++i) {
        for (int j = 0; j < slices; ++j) {
            uint32_t a = (uint32_t)(i * stride + j);
            uint32_t b = (uint32_t)(a + stride);
            mesh.indices.push_back(a);
            mesh.indices.push_back(b);
            mesh.indices.push_back(a + 1);
            mesh.indices.push_back(a + 1);
            mesh.indices.push_back(b);
            mesh.indices.push_back(b + 1);
        }
    }
    return mesh;
}
