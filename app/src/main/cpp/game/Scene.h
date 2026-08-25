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

    // Стройка (G3-B): выбор типа -> призрак перед героем на клетке сетки -> подтверждение.
    // Размещение авторитетно на сервере (клиент лишь шлёт запрос и рисует превью).
    void beginBuild(int type);
    void cancelBuild() { buildActive_ = false; }
    void confirmBuild();                       // отправить запрос постройки на клетку призрака
    bool buildMode() const { return buildActive_; }
    int buildType() const { return (int)buildType_; }
    bool buildGhostValid() const;              // клетка призрака валидна (клиентская оценка)
    const BuildingInfo* buildInfo(int type) const;  // имя/стоимость/параметры типа из конфига

    void setUiScale(float s);  // масштаб джойстика под DPI

    // Сеть.
    void hostGame();
    void joinGame(const char* ip);
    void leaveGame();
    bool netConnected() const { return client_.connected(); }
    bool netHost() const { return host_; }
    // Диагностика соединения (для HUD/панели): пинг + производные от статуса флаги.
    int netPingMs() const { return client_.pingMs(); }        // RTT до сервера, мс (-1 нет)
    bool netConnecting() const { return client_.status() == NetStatus::Connecting; }
    bool netConnectionLost() const { return client_.status() == NetStatus::Lost; }
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
    // Ставки своего героя.
    float heroHp() const { return localHp_; }
    float heroMaxHp() const { return localMaxHp_; }
    bool heroDead() const { return client_.connected() && localHp_ <= 0.0f; }
    float heroRespawnLeft() const { return localRespawn_; }  // секунд до респауна

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

    // Клиентский визуал/пикинг по типу сущности — ОДНА таблица вместо разбросанных switch
    // (рендер, пикинг, призрак стройки читают её; yOffset больше НЕ дублируется). Заполняется
    // в build(); Hero остаётся default (mesh=0 — рисуется отдельно, скиннинг-лиса).
    struct EntityVisual {
        MeshHandle mesh = 0;         // 0 = не рисуется generic-путём
        MaterialHandle material = 0;
        float yOffset = 0.0f;        // подъём центра над позицией (рендер И пикинг)
        float pickRadius = 0.0f;     // радиус сферы пикинга
        bool pickable = false;       // выбирается кликом (Hero — нет)
        bool building = false;       // занимает клетку сетки (для стройки)
    };
    static constexpr int kEntityVisualCount = (int)EntityType::Core + 1;  // Core — последний тип
    EntityVisual visuals_[kEntityVisualCount];
    const EntityVisual& visual(EntityType t) const;  // доступ по типу (вне диапазона -> пусто)
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
    Grid grid_;                // строительная сетка (из описания сцены; та же, что у сервера)
    uint32_t selectedId_ = 0;  // id выделенной кликом сущности (0 = нет)
    uint8_t localTeam_ = 0;    // команда своего героя (из снапшота) — для ресурса per-team
    float localHp_ = 1.0f;      // hp своего героя (>0 = жив); из снапшота (вне сессии — «жив»)
    float localMaxHp_ = 100.0f; // макс. hp героя (из конфига)
    float localRespawn_ = 0.0f; // отсчёт респауна при поверженном (из aux сущности)

    // Стройка: активный режим + выбранный тип + материалы призрака (валид/невалид).
    bool buildActive_ = false;
    EntityType buildType_ = EntityType::Tower;
    MaterialHandle ghostOkMat_ = 0, ghostBadMat_ = 0;
    // Призрак: клетка перед героем + мировой центр + валидность. Возвращает валидность.
    bool computeGhost(int& cx, int& cz, Vec3& center) const;
    bool host_ = false;
    // Авто-реконнект для join-сессии (host к 127.0.0.1 не переподключаем). serverIp_
    // запоминается в joinGame; при статусе Lost повторяем connect раз в kReconnectPeriod.
    char serverIp_[64] = {0};
    bool wantReconnect_ = false;
    float reconnectTimer_ = 0.0f;
    uint32_t inputSeq_ = 0;
    std::vector<RemoteEntity> remoteEntities_;  // все чужие сущности (герои/здания/…)
    std::vector<PendingInput> pending_;  // неподтверждённые вводы (для реплея)
    double simClock_ = 0.0;              // часы симуляции (сек)
    float tickDt_ = kTickDt;             // длительность тика (единый шаг, из engine/net/Net.h)

    void applySnapshot();
};
