#pragma once

#include <vector>

#include "AssetSource.h"
#include "Character.h"
#include "FollowCamera.h"
#include "Input.h"
#include "MathUtil.h"
#include "Mesh.h"
#include "Model.h"
#include "Net.h"
#include "RenderFrame.h"
#include "Texture.h"

class Renderer;

// Снапшот состояния во времени (для буфера интерполяции чужих игроков).
struct TimedState {
    double t = 0.0;  // время приёма (по часам симуляции), сек
    Vec3 pos{0.0f, 0.0f, 0.0f};
    float yaw = 0.0f;
    float anim = 0.0f;
};

// Другой игрок: сущность + буфер снапшотов (рендерим с задержкой, интерполируя).
struct RemotePlayer {
    uint32_t id = 0;
    Character ch;
    std::vector<TimedState> buffer;
};

// Отправленная, но ещё не подтверждённая сервером команда (для реплея).
struct PendingInput {
    InputCommand cmd;
    float dt = 0.0f;
};

// Позиция/поворот/масштаб статичного объекта окружения.
struct Transform {
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 rotation{0.0f, 0.0f, 0.0f};
    Vec3 scale{1.0f, 1.0f, 1.0f};

    Mat4 matrix() const {
        return Mat4::translation(position)
             * Mat4::rotationY(rotation.y)
             * Mat4::rotationX(rotation.x)
             * Mat4::rotationZ(rotation.z)
             * Mat4::scale(scale);
    }
};

// Статичный объект окружения (пол, кубы, сферы, кольцо).
struct GameObject {
    Transform transform;
    MeshHandle mesh = 0;
    MaterialHandle material = 0;
    float spin = 0.0f;
    float prevRotY = 0.0f;  // для интерполяции вращения между тиками
};

/**
 * Игровой мир. Владеет окружением, управляемым персонажем, следящей камерой и
 * джойстиком. Камера следует за персонажем через обобщённый интерфейс (позиция
 * + facing), а не за «лисой» — актора можно заменить/добавить без правок камеры.
 */
class Scene {
public:
    // Построить сцену из файла (scenePath — через AssetSource, напр. "scenes/default.scene").
    void build(Renderer& renderer, AssetSource& assets,
               const char* scenePath = "scenes/default.scene");

    // Шаг симуляции на фиксированный dt (движение, анимация, вращение декора).
    void fixedUpdate(float dt);

    // Кадр с интерполяцией между тиками (alpha 0..1) + сглаживание камеры (renderDt).
    RenderFrame render(float alpha, float aspect, float renderDt);

    void onPointer(float x, float y, bool pressed);  // -> джойстик (тач)
    void setMoveInput(float x, float y) { extX_ = x; extY_ = y; }  // внешняя ось (клавиатура)

    void setUiScale(float s);  // масштаб джойстика под DPI

    // Сеть.
    void hostGame();
    void joinGame(const char* ip);
    void leaveGame();
    bool netConnected() const { return client_.connected(); }
    bool netHost() const { return host_; }
    int remoteCount() const { return (int)remotes_.size(); }

    // Для ImGui/HUD.
    const VirtualJoystick& joystick() const { return joystick_; }
    float characterSpeed() const { return player_.speed01; }

    float modelScale() const { return foxScale_; }
    void setModelScale(float s) { foxScale_ = s; }
    float modelYawOffset() const { return foxYawOffset_; }
    void setModelYawOffset(float y) { foxYawOffset_ = y; }
    float cameraDistance() const { return camera_.distance; }
    void setCameraDistance(float d) { camera_.distance = d; }
    float cameraHeight() const { return camera_.height; }
    void setCameraHeight(float h) { camera_.height = h; }

    Vec3 lightDir() const { return lightDir_; }
    void setLightDir(Vec3 d) { lightDir_ = d; }

private:
    FollowCamera camera_;
    std::vector<GameObject> objects_;

    SkinnedModel foxModel_;   // данные модели (общие для всех аватаров-лис)
    SkinnedHandle foxMesh_ = 0;   // GPU-меш (клиентский ресурс)
    TextureHandle foxTex_ = 0;
    float foxScale_ = 0.03f;
    float foxYawOffset_ = 0.0f;
    Character player_;        // управляемый актор (симуляция)
    VirtualJoystick joystick_;
    float extX_ = 0.0f, extY_ = 0.0f;  // внешняя ось движения (клавиатура на десктопе)
    float uiScale_ = 1.0f;
    Vec3 lightDir_{0.4f, 1.0f, 0.6f};  // направление НА свет (из файла сцены)

    // Построить отрисовочный предмет лисы по состоянию (клиентский рендер).
    SkinnedItem makeFoxItem(Vec3 pos, float yaw, float animParam, float animTime) const;

    // Сеть.
    NetClient client_;
    NetServer server_;
    bool host_ = false;
    uint32_t inputSeq_ = 0;
    std::vector<RemotePlayer> remotes_;
    std::vector<PendingInput> pending_;  // неподтверждённые вводы (для реплея)
    double simClock_ = 0.0;              // часы симуляции (сек)
    float tickDt_ = 1.0f / 30.0f;        // длительность тика (для расчёта времени рендера)

    void applySnapshot();
};
