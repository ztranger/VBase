#pragma once

#include <memory>
#include <vector>

#include "engine/assets/AssetSource.h"
#include "game/BuildingConfig.h"
#include "game/Character.h"
#include "engine/core/FollowCamera.h"
#include "engine/core/Input.h"
#include "engine/core/MathUtil.h"
#include "engine/assets/Mesh.h"
#include "engine/assets/Model.h"
#include "engine/net/Net.h"
#include "engine/core/RenderFrame.h"
#include "game/SceneDesc.h"
#include "engine/core/Texture.h"

class Renderer;
class CollisionWorld;

// Снапшот состояния во времени (для буфера интерполяции чужих игроков).
struct TimedState {
    double t = 0.0;  // время приёма (по часам симуляции), сек
    Vec3 pos{0.0f, 0.0f, 0.0f};
    float yaw = 0.0f;
    float anim = 0.0f;
};

// Чужая сущность (не свой герой): любой тип из снапшота — герой, генератор,
// хранилище, враг, … Рендерим по типу; подвижные интерполируем через буфер.
struct RemoteEntity {
    uint32_t id = 0;
    uint8_t type = 0;   // EntityType
    uint8_t team = 0;
    Character ch;       // трансформ для рендера/интерполяции
    std::vector<TimedState> buffer;
    float aux = 0.0f;   // ресурс в хранилище и т.п. (последнее значение, без интерполяции)
    float hp = 0.0f;    // здоровье (ядро/враг) из снапшота
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
    Scene();
    ~Scene();  // из-за unique_ptr<CollisionWorld> (неполный тип в заголовке)

    // Построить сцену из файла (scenePath — через AssetSource, напр. "scenes/default.scene").
    void build(Renderer& renderer, AssetSource& assets,
               const char* scenePath = "scenes/default.scene");

    // Шаг симуляции на фиксированный dt (движение, анимация, вращение декора).
    void fixedUpdate(float dt);

    // Кадр с интерполяцией между тиками (alpha 0..1) + сглаживание камеры (renderDt).
    RenderFrame render(float alpha, float aspect, float renderDt);

    void onPointer(float x, float y, bool pressed);  // -> левый джойстик (одиночный тач/десктоп)
    void setMoveInput(float x, float y) { extX_ = x; extY_ = y; }  // внешняя ось героя (WASD)
    void setCameraInput(float yawAxis, float zoomAxis) {           // внешняя ось камеры (стрелки)
        extCamYaw_ = yawAxis;
        extCamZoom_ = zoomAxis;
    }
    void requestJump() { jumpQueued_ = true; }  // прыжок на следующем тике (клавиша/кнопка)

    // Мультитач (Android twin-stick): левая половина экрана — левый стик (герой),
    // правая — правый стик (камера). Каждый стик держит палец по его id.
    void onTouchDown(int id, float x, float y, float vw, float vh);
    void onTouchMove(int id, float x, float y);
    void onTouchUp(int id);

    // Клик/тап (пиксели x,y в вьюпорте vw×vh) -> raycast по зданиям, выделение для панели.
    void onClick(float x, float y, float vw, float vh);
    void clearSelection() { selectedId_ = 0; }
    // Для панели информации о выделенном здании (GameUi).
    int selectedEntityType() const;         // EntityType или -1 (нет выделения)
    const BuildingInfo* selectedInfo() const;  // тексты/параметры из конфига (или nullptr)
    float selectedAux() const;              // динамика (ресурс в хранилище и т.п.)

    void setUiScale(float s);  // масштаб джойстика под DPI

    // Сеть.
    void hostGame();
    void joinGame(const char* ip);
    void leaveGame();
    bool netConnected() const { return client_.connected(); }
    bool netHost() const { return host_; }
    int remoteCount() const;  // число ДРУГИХ героев (без зданий/врагов)

    // Для ImGui/HUD.
    const VirtualJoystick& joystick() const { return joystick_; }         // левый стик (герой)
    const VirtualJoystick& cameraJoystick() const { return camJoystick_; }  // правый стик (камера)
    float characterSpeed() const { return player_.speed01; }
    float resourceCurrent() const;  // сумма ресурса во всех хранилищах (из снапшотов)
    float resourceCap() const;      // суммарная ёмкость хранилищ (из описания сцены)

    // Бой / жизненный цикл матча (для HUD).
    int matchPhase() const;    // GamePhase (0 Playing / 1 Won / 2 Lost)
    float coreHp() const;      // текущее здоровье ядра (из снапшотов; -1 если ядра нет)
    float coreMaxHp() const;   // максимум ядра (из конфига)

    float modelScale() const { return foxScale_; }
    void setModelScale(float s) { foxScale_ = s; }
    float modelYawOffset() const { return foxYawOffset_; }
    void setModelYawOffset(float y) { foxYawOffset_ = y; }
    float cameraDistance() const { return camera_.distance; }
    void setCameraDistance(float d) { camera_.distance = d; }
    float cameraPitch() const { return camera_.pitch; }
    void setCameraPitch(float p) { camera_.pitch = p; }

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

    // Визуалы сущностей базы/врагов (клиентский рендер по типу; создаются в build).
    MeshHandle genMesh_ = 0, storMesh_ = 0, spawnMesh_ = 0, coreMesh_ = 0, enemyMesh_ = 0, towerMesh_ = 0;
    MaterialHandle genMat_ = 0, storMat_ = 0, spawnMat_ = 0, coreMat_ = 0, enemyMat_ = 0, towerMat_ = 0;
    Character player_;        // управляемый актор (симуляция)
    std::unique_ptr<CollisionWorld> collision_;  // кинематическая физика (Jolt)
    VirtualJoystick joystick_;         // левый стик — движение героя
    VirtualJoystick camJoystick_;      // правый стик — камера (Android)
    int movePointer_ = -1;             // id пальца, владеющего левым стиком (-1 нет)
    int camPointer_ = -1;              // id пальца, владеющего правым стиком
    float extX_ = 0.0f, extY_ = 0.0f;  // внешняя ось движения героя (WASD на десктопе)
    float extCamYaw_ = 0.0f, extCamZoom_ = 0.0f;  // внешняя ось камеры (стрелки на десктопе)
    bool jumpQueued_ = false;          // запрошен прыжок (сбрасывается в fixedUpdate)
    float uiScale_ = 1.0f;
    Vec3 lightDir_{0.4f, 1.0f, 0.6f};  // направление НА свет (из файла сцены)

    // Построить отрисовочный предмет лисы по состоянию (клиентский рендер).
    SkinnedItem makeFoxItem(Vec3 pos, float yaw, float animParam, float animTime) const;

    // Сеть.
    NetClient client_;
    NetServer server_;
    SceneDesc sceneDesc_;      // сохранённое описание (host-режим отдаёт его серверу)
    BuildingConfig config_;    // параметры/тексты типов зданий (из конфига)
    uint32_t selectedId_ = 0;  // id выделенной кликом сущности (0 = нет)
    bool host_ = false;
    uint32_t inputSeq_ = 0;
    std::vector<RemoteEntity> remoteEntities_;  // все чужие сущности (герои/здания/…)
    std::vector<PendingInput> pending_;  // неподтверждённые вводы (для реплея)
    double simClock_ = 0.0;              // часы симуляции (сек)
    float tickDt_ = kTickDt;             // длительность тика (единый шаг, из engine/net/Net.h)

    void applySnapshot();
};
