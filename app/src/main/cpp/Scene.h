#pragma once

#include <android/asset_manager.h>

#include <vector>

#include "Camera.h"
#include "MathUtil.h"
#include "Mesh.h"
#include "Model.h"
#include "RenderFrame.h"
#include "Texture.h"

class Renderer;

// Позиция/поворот/масштаб объекта. matrix() собирает world-матрицу.
struct Transform {
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 rotation{0.0f, 0.0f, 0.0f};  // эйлеровы углы, радианы
    Vec3 scale{1.0f, 1.0f, 1.0f};

    Mat4 matrix() const {
        return Mat4::translation(position)
             * Mat4::rotationY(rotation.y)
             * Mat4::rotationX(rotation.x)
             * Mat4::rotationZ(rotation.z)
             * Mat4::scale(scale);
    }
};

// Игровой объект = трансформ + какой меш + какой материал + простая анимация.
struct GameObject {
    Transform transform;
    MeshHandle mesh = 0;
    MaterialHandle material = 0;
    float spin = 0.0f;  // рад/сек вокруг Y (0 — не вращается)
};

/**
 * Игровой мир. Владеет объектами и камерой, обновляет их логику и умеет
 * собрать RenderFrame для рендера. Не зависит от GL/Vulkan.
 */
class Scene {
public:
    // Создаёт меши/текстуры/материалы через рендер и грузит модели из ассетов.
    void build(Renderer& renderer, AAssetManager* assets);

    void update(float dt);

    // Ввод из main -> вращение камеры (это логика сцены, не рендера).
    void onPointer(float x, float y, bool pressed);

    RenderFrame buildFrame(float aspect) const;

    // Управление анимированной моделью (для ImGui в main).
    int animationCount() const { return (int)foxModel_.animations.size(); }
    const char* animationName(int i) const;
    int currentAnimation() const { return foxAnim_; }
    void setAnimation(int i);
    float modelScale() const { return foxScale_; }
    void setModelScale(float s) { foxScale_ = s; }

private:
    Camera camera_;
    std::vector<GameObject> objects_;

    // Анимированная модель (glTF + скиннинг).
    SkinnedModel foxModel_;
    SkinnedHandle foxMesh_ = 0;
    int foxAnim_ = 0;
    float foxTime_ = 0.0f;
    float foxScale_ = 0.03f;

    bool dragging_ = false;
    float lastX_ = 0.0f;
    float lastY_ = 0.0f;
};
