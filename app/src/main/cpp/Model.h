#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "AssetSource.h"
#include "MathUtil.h"
#include "Texture.h"

// Вершина скиннинг-меша: геометрия + до 4 костей с весами.
struct SkinnedVertex {
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
    float joints[4];   // индексы костей (храним как float для простого атрибута)
    float weights[4];  // веса влияния костей (сумма ~1)
};

using SkinnedHandle = uint32_t;  // 0 — невалидный

// Узел сцены glTF: базовый TRS + родитель (для иерархии).
struct ModelNode {
    Vec3 t;
    Quat r;
    Vec3 s{1.0f, 1.0f, 1.0f};
    int parent = -1;
};

// Канал анимации: как во времени меняется одно свойство (T/R/S) одного узла.
struct AnimChannel {
    int node = -1;
    int path = 0;   // 0 = translation, 1 = rotation, 2 = scale
    bool step = false;
    std::vector<float> times;
    std::vector<float> values;  // 3 (T/S) или 4 (R) значения на ключ
};

struct Animation {
    std::string name;
    float duration = 0.0f;
    std::vector<AnimChannel> channels;
};

// Скиннинг-модель: геометрия + скелет + анимации. Чистые данные, без GPU.
struct SkinnedModel {
    static constexpr int kMaxJoints = 64;

    std::vector<SkinnedVertex> vertices;
    std::vector<uint32_t> indices;

    std::vector<ModelNode> nodes;      // все узлы glTF
    std::vector<int> jointNodes;       // индекс узла для каждой кости
    std::vector<Mat4> inverseBind;     // обратная bind-матрица на кость

    std::vector<Animation> animations;

    TextureData baseColor;      // альбедо-текстура из glb (если есть)
    bool hasTexture = false;

    // Поза узлов (локальные T/R/S) для анимации animIndex во времени time (сек).
    void samplePose(int animIndex, float time,
                    std::vector<Vec3>& T, std::vector<Quat>& R, std::vector<Vec3>& S) const;

    // Матрицы костей одной анимации (для uJoints[] в шейдере).
    void sampleAnimation(int animIndex, float time, std::vector<Mat4>& out) const;

    // Блендинг двух анимаций по фактору blend (0 -> A, 1 -> B). Смешивается ПОЗА
    // (T/S линейно, R через slerp), затем считаются матрицы костей.
    void sampleBlend(int animA, float timeA, int animB, float timeB,
                     float blend, std::vector<Mat4>& out) const;
};

// Загрузка .glb/.gltf через cgltf (данные читаются из AssetSource). Первый скин + анимации.
bool loadGltfModel(AssetSource& src, const char* path, SkinnedModel& out);
