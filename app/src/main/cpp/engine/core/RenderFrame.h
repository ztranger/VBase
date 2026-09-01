#pragma once

#include <functional>
#include <string>
#include <vector>

#include "engine/core/MathUtil.h"
#include "engine/assets/Mesh.h"
#include "engine/assets/Model.h"
#include "engine/core/Texture.h"

// Единственный контракт между игровым слоём и рендером.
// Игра каждый кадр собирает RenderFrame, рендер его рисует и больше
// ничего про «игру» не знает.

struct RenderItem {
    MeshHandle mesh = 0;
    MaterialHandle material = 0;  // цвет/текстура вынесены в материал
    Mat4 model;                   // world-матрица объекта
};

// 2D-текст поверх сцены (HUD). Координаты в пикселях от левого верхнего угла.
struct HudText {
    std::string text;
    float x = 0.0f;
    float y = 0.0f;
    float pixelHeight = 32.0f;
    Vec3 color{1.0f, 1.0f, 1.0f};
};

// Скиннинг-объект: анимированная модель с матрицами костей на этот кадр.
struct SkinnedItem {
    SkinnedHandle mesh = 0;
    TextureHandle texture = 0;   // альбедо (0 -> белая, останется чистый цвет)
    Mat4 model;                  // world-размещение
    Vec3 color{1.0f, 1.0f, 1.0f};
    std::vector<Mat4> joints;    // матрицы костей (uJoints[] в шейдере)
};

struct RenderFrame {
    Mat4 view;
    Mat4 proj;
    Vec3 cameraPos;                   // позиция камеры (нужна Phong-шейдеру для блика)
    Vec3 lightDir{0.4f, 1.0f, 0.6f};  // направление НА источник света

    // Тени (directional shadow map). Правятся слайдерами в DebugPanel.
    bool shadowsEnabled = true;
    float shadowBias = 0.0025f;   // сдвиг глубины против self-shadow acne
    float shadowRadius = 14.0f;   // полуширина орто-коробки света (охват арены)

    std::vector<RenderItem> items;
    std::vector<SkinnedItem> skinned; // анимированные модели (скиннинг)
    std::vector<HudText> hud;         // оверлей-текст (рисуется после 3D)

    float deltaTime = 0.0f;           // для ImGui (io.DeltaTime)
    std::function<void()> ui;         // построение ImGui-виджетов (между NewFrame и Render)
};
