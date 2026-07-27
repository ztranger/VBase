#include "Mesh.h"

#include <cmath>

MeshData makePlane(float size, float uvTiles) {
    float h = size * 0.5f;
    float t = uvTiles;
    MeshData mesh;
    // Четыре угла на y=0, нормаль вверх. UV тайлятся uvTiles раз (для чекера).
    mesh.vertices = {
        {-h, 0.0f, -h, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f},
        { h, 0.0f, -h, 0.0f, 1.0f, 0.0f, t,    0.0f},
        { h, 0.0f,  h, 0.0f, 1.0f, 0.0f, t,    t   },
        {-h, 0.0f,  h, 0.0f, 1.0f, 0.0f, 0.0f, t   },
    };
    mesh.indices = {0, 2, 1, 0, 3, 2};
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
        uint32_t base = (uint32_t)mesh.vertices.size();
        for (int v = 0; v < 4; ++v) {
            mesh.vertices.push_back({
                corners[f][v][0], corners[f][v][1], corners[f][v][2],
                n[f][0], n[f][1], n[f][2],
                uv[v][0], uv[v][1]});
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
            mesh.vertices.push_back({nx * radius, ny * radius, nz * radius,
                                     nx, ny, nz, uu, vv});
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
