#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include "engine/assets/AssetSource.h"
#include "engine/audio/Audio.h"  // SoundId/SoundEvent (портативный словарь; без аудио-движка)
#include "game/BuildingConfig.h"
#include "game/Character.h"
#include "game/ClientSession.h"  // P2-18 шаг 3: сетевая сессия (client/server/reconnect)
#include "game/ClientWorld.h"    // P2-18 шаг 4: физика + снапшоты + предсказание
#include "engine/core/FollowCamera.h"
#include "engine/core/Input.h"
#include "engine/core/MathUtil.h"
#include "engine/assets/Mesh.h"
#include "engine/assets/Model.h"
#include "engine/net/Net.h"
#include "engine/core/RenderFrame.h"
#include "game/NavDebug.h"
#include "game/SceneDesc.h"
#include "game/SceneTypes.h"  // P2-18: plain-структуры данных сцены (вынесены из этого заголовка)
#include "game/WorldPresentation.h"  // P2-18 шаг 2: боевая косметика / звук / вью-матрицы
#include "engine/core/Texture.h"

class Renderer;
class CollisionWorld;
struct CharacterDesc;  // game/CharacterRoster.h (ростер персонажей/мобов)

// Структуры данных (TimedState/RemoteEntity/PendingInput/Transform/GameObject/PlayerModel/
// EntityVisual/DyingMob/SceneEntry/CombatMarker/DamageNumber/ImpactSpark/BuildPoof) — в
// game/SceneTypes.h (P2-18 шаг 1). Ниже — только сам класс Scene.

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

    // Пересоздать ТОЛЬКО GPU-ресурсы под новый рендер, сохранив игровую сессию (сеть, матч,
    // предсказание, физика). Нужно при потере/пересоздании окна на Android и смене бэкенда:
    // рендер уничтожается, а `Scene` (с client_/server_/remoteEntities_) живёт (см. P1-07).
    // Требует ранее выполненного build() (переиспользует сохранённые sceneDesc_/config_/grid_).
    void rebuildGraphics(Renderer& renderer, AssetSource& assets);

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
    void requestAttack() { attackQueued_ = true; }  // атака (каст) на следующем тике

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
    void joinGame(const char* ip, uint16_t port = kNetPort);
    void leaveGame();
    bool netConnected() const { return session_.connected(); }
    bool netHost() const { return session_.isHost(); }
    // Диагностика соединения (для HUD/панели): пинг + производные от статуса флаги.
    int netPingMs() const { return session_.pingMs(); }        // RTT до сервера, мс (-1 нет)
    bool netConnecting() const { return session_.status() == NetStatus::Connecting; }
    bool netConnectionLost() const { return session_.status() == NetStatus::Lost; }
    const char* netServerAddress() const { return session_.serverAddress(); }  // цель join (для UI)
    int netServerPort() const { return session_.serverPort(); }
    int netReconnectAttempts() const { return session_.reconnectAttempts(); }  // попыток реконнекта
    void netRetryNow() { session_.retryNow(); }  // форсировать реконнект сейчас
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
    bool heroDead() const { return session_.connected() && localHp_ <= 0.0f; }
    float heroRespawnLeft() const { return localRespawn_; }  // секунд до респауна

    // Читаемость боя: worldspace HP-бары + всплывающие числа урона. Scene (game/) только
    // ПРОИЗВОДИТ данные (диф hp из снапшотов, без ImGui); рисует GameUi (engine/render),
    // проецируя мировые точки через view/proj. Чистая клиентская косметика — протокол не трогает.
    // Определения — в game/SceneTypes.h; алиасы для совместимости с внешним `Scene::X` (GameUi).
    using CombatMarker = ::CombatMarker;
    using DamageNumber = ::DamageNumber;
    using ImpactSpark = ::ImpactSpark;
    using BuildPoof = ::BuildPoof;
    const std::vector<CombatMarker>& combatMarkers() const { return presentation_.markers; }
    const std::vector<DamageNumber>& damageNumbers() const { return presentation_.damageNumbers; }
    const std::vector<ImpactSpark>& impactSparks() const { return presentation_.sparks; }
    const std::vector<BuildPoof>& buildPoofs() const { return presentation_.poofs; }
    // Мировая позиция выделенного кликом здания (ринг выделения в оверлее). false = нет выделения.
    bool selectedWorldPos(Vec3& out) const;
    const Mat4& viewMatrix() const { return presentation_.lastView; }  // матрицы последнего кадра боя
    const Mat4& projMatrix() const { return presentation_.lastProj; }

    // Звуковые события за кадр: Scene накапливает из снапшотов/действий, платформа сливает и
    // проигрывает через Audio, затем чистит. Тут только словарь SoundId — без аудио-движка.
    const std::vector<SoundEvent>& sounds() const { return presentation_.sounds; }
    void clearSounds() { presentation_.sounds.clear(); }

    float modelScale() const { return chars_.empty() ? 1.0f : chars_[localCharIndex_].scale; }
    void setModelScale(float s) { if (!chars_.empty()) chars_[localCharIndex_].scale = s; }
    float modelYawOffset() const { return chars_.empty() ? 0.0f : chars_[localCharIndex_].yawOffset; }
    void setModelYawOffset(float y) { if (!chars_.empty()) chars_[localCharIndex_].yawOffset = y; }

    // Выбор персонажа (экран CharacterSelect).
    int rosterCount() const { return (int)chars_.size(); }
    const char* rosterName(int i) const {
        return (i >= 0 && i < (int)chars_.size()) ? chars_[i].name.c_str() : "";
    }
    int selectedCharacter() const { return localCharIndex_; }
    void selectCharacter(int i);  // локальный персонаж + уведомить сервер (для рендера чужими)

    // Список доступных сцен (config/scenes.cfg) для выбора в меню. Смена сцены —
    // перезагрузкой мира платформой (десктоп: пересоздание рендера; Android: рекриэйт),
    // поэтому сам Scene только перечисляет; путь текущей — currentScenePath().
    int sceneListCount() const { return (int)sceneList_.size(); }
    const char* sceneListName(int i) const {
        return (i >= 0 && i < (int)sceneList_.size()) ? sceneList_[i].name.c_str() : "";
    }
    const char* sceneListPath(int i) const {
        return (i >= 0 && i < (int)sceneList_.size()) ? sceneList_[i].path.c_str() : "";
    }
    const char* currentScenePath() const { return currentScenePath_.c_str(); }
    int currentSceneIndex() const {  // индекс текущей сцены в списке; -1 если нет в манифесте
        for (int i = 0; i < (int)sceneList_.size(); ++i)
            if (sceneList_[i].path == currentScenePath_) return i;
        return -1;
    }

    // Рендер вне боя (главный цикл выбирает путь по UiMode): 3D-превью выбранного
    // персонажа (экран выбора) и пустой фон меню (мир не показываем).
    RenderFrame renderCharacterPreview(float alpha, float aspect, float renderDt);
    RenderFrame renderMenuBackdrop(float aspect);
    float cameraDistance() const { return camera_.distance; }
    void setCameraDistance(float d) { camera_.distance = d; }
    float cameraPitch() const { return camera_.pitch; }
    void setCameraPitch(float p) { camera_.pitch = p; }

    Vec3 lightDir() const { return lightDir_; }
    void setLightDir(Vec3 d) { lightDir_ = d; }

    // Тени (directional shadow map). Правятся слайдерами в DebugPanel, уходят в RenderFrame.
    bool shadowsEnabled() const { return shadowsEnabled_; }
    void setShadowsEnabled(bool e) { shadowsEnabled_ = e; }
    float shadowBias() const { return shadowBias_; }
    void setShadowBias(float b) { shadowBias_ = b; }
    float shadowRadius() const { return shadowRadius_; }
    void setShadowRadius(float r) { shadowRadius_ = r; }

    // Туман по глубине (fogColor — в линейном пространстве, как ждёт шейдер).
    Vec3 fogColor() const { return fogColor_; }
    void setFogColor(Vec3 c) { fogColor_ = c; }
    float fogDensity() const { return fogDensity_; }
    void setFogDensity(float d) { fogDensity_ = d; }

    // Отладочный оверлей навигации (сетка / обстаклы / поле потока к целям). Клиент
    // реконструирует навсетку из снапшотов теми же NavGrid/FlowField, что и авторитетный
    // сервер, — без нового сетевого пакета. Рисует GameUi поверх кадра (см. BattleScreen).
    bool navDebugEnabled() const { return navDebug_; }
    void setNavDebugEnabled(bool e);
    const NavDebugFrame& navDebugFrame() const { return navDebugFrame_; }

private:
    FollowCamera camera_;
    std::vector<GameObject> objects_;

    // Манифест доступных сцен (config/scenes.cfg): путь + отображаемое имя. Меню
    // перечисляет их и просит платформу перезагрузить мир выбранной. (SceneEntry — SceneTypes.h)
    std::vector<SceneEntry> sceneList_;
    std::string currentScenePath_;     // путь сцены, которой построен этот Scene
    void loadSceneManifest(AssetSource& assets, const char* path);  // config/scenes.cfg -> sceneList_

    // Реестр выбираемых персонажей (грузится в build из config/characters.cfg). ВСЕ модели
    // грузятся один раз -> мгновенное переключение и рендер чужих их моделями без утечек GPU
    // (у Renderer нет удаления мешей/текстур). Индексы клипов резолвятся ПО ИМЕНИ. PlayerModel —
    // в game/SceneTypes.h.
    std::vector<PlayerModel> chars_;   // ростер героев; индекс = charType в снапшоте (сетевой контракт)
    std::vector<PlayerModel> mobs_;    // ростер мобов (config/enemies.cfg); индекс = enemy.charType
    int localCharIndex_ = 0;           // выбранный локальным игроком
    float previewSpin_ = 0.0f;         // накопленный угол вращения модели на экране выбора

    // Боевая косметика (HP-бары/числа урона/искры/вспышки/тряска), звук, «трупы» мобов и вью-
    // матрицы последнего кадра — в WorldPresentation (P2-18 шаг 2). Scene ПРОИЗВОДИТ их в
    // applySnapshot/render и раздаёт геттерами; GameUi рисует.
    WorldPresentation presentation_;

    // Снаряды башен теперь СЕРВЕРНЫЕ сущности (EntityType::Projectile в снапшотах) — рисуем их
    // в remote-цикле этим мешем/материалом (болт). Клиентская симуляция снаряда убрана.
    MeshHandle projMesh_ = 0;        // меш болта (единичный куб, масштабируется в вытянутый)
    MaterialHandle projMat_ = 0;     // материал болта (Unlit, свечение)

    // Клиентский визуал/пикинг по типу сущности — ОДНА таблица вместо разбросанных switch
    // (рендер, пикинг, призрак стройки читают её; yOffset больше НЕ дублируется). Заполняется
    // в build(); Hero остаётся default (mesh=0 — рисуется отдельно, скиннинг). EntityVisual —
    // в game/SceneTypes.h.
    static constexpr int kEntityVisualCount = (int)EntityType::Core + 1;  // Core — последний тип
    EntityVisual visuals_[kEntityVisualCount];
    const EntityVisual& visual(EntityType t) const;  // доступ по типу (вне диапазона -> пусто)
    Character player_;         // управляемый актор (симуляция; хэндл капсулы — в clientWorld_)
    ClientWorld clientWorld_;  // физика + снапшоты + предсказание/реконсиляция (P2-18 шаг 4)
    VirtualJoystick joystick_;         // левый стик — движение героя
    VirtualJoystick camJoystick_;      // правый стик — камера (Android)
    int movePointer_ = -1;             // id пальца, владеющего левым стиком (-1 нет)
    int camPointer_ = -1;              // id пальца, владеющего правым стиком
    float extX_ = 0.0f, extY_ = 0.0f;  // внешняя ось движения героя (WASD на десктопе)
    float extCamYaw_ = 0.0f, extCamZoom_ = 0.0f;  // внешняя ось камеры (стрелки на десктопе)
    bool jumpQueued_ = false;          // запрошен прыжок (сбрасывается в fixedUpdate)
    bool attackQueued_ = false;        // запрошена атака/каст (сбрасывается в fixedUpdate)
    float uiScale_ = 1.0f;
    Vec3 lightDir_{0.4f, 1.0f, 0.6f};  // направление НА свет (из файла сцены)
    bool shadowsEnabled_ = true;       // тени (directional shadow map)
    float shadowBias_ = 0.0025f;       // сдвиг глубины против self-shadow acne
    float shadowRadius_ = 14.0f;       // полуширина орто-коробки света (охват арены)
    Vec3 fogColor_{0.09f, 0.13f, 0.20f};  // цвет тумана (линейное пространство)
    float fogDensity_ = 0.014f;        // плотность экспоненциального тумана (0 = выкл)

    // Отладочная навигация: снимок навсетки+поля потока. Перестраивается из снапшотов
    // ТОЛЬКО когда оверлей включён и изменился набор зданий/ядер (дешёвая подпись
    // navDebugSig_ гасит лишние BFS на больших полях — здания статичны, поле не меняется).
    bool navDebug_ = false;
    NavDebugFrame navDebugFrame_;
    uint64_t navDebugSig_ = 0;
    void rebuildNavDebug();  // собрать navDebugFrame_ из remoteEntities_/sceneDesc_ (см. NavDebug)

    // Построить отрисовочный предмет модели reg[index] по состоянию (клиентский рендер).
    // reg — chars_ (герои) или mobs_ (враги). oneShotClip>=0 — проиграть конкретный клип в
    // oneShotTime (для «трупа»: смерть), перекрывая локомоцию/атаку.
    SkinnedItem makeSkinnedItem(const std::vector<PlayerModel>& reg, int index, Vec3 pos, float yaw,
                                float animParam, float animTime, float attackTime = 0.0f,
                                int oneShotClip = -1, float oneShotTime = 0.0f,
                                float locoPhase = -1.0f) const;  // -1 = нет (walk<->run возьмёт animTime)
    // Скорость набега фазы локомоции (циклов/сек) по animParam: каденс walk->run.
    float locoRate(const std::vector<PlayerModel>& reg, int index, float animParam) const;
    // Загрузить модели ростера в GPU-реестр (клипы по имени). Используется для героев и мобов.
    void loadRosterModels(Renderer& renderer, AssetSource& assets,
                          const std::vector<CharacterDesc>& roster, std::vector<PlayerModel>& out);
    // GPU-часть сборки (общая для build() и rebuildGraphics()): создаёт ресурсы рендера из
    // сохранённого sceneDesc_/config_/grid_, не трогая физику/сеть/сессию.
    void createGpuResources(Renderer& renderer, AssetSource& assets);

    // Сеть — за фасадом ClientSession (транспорт + host-сервер + реконнект). См. P2-18 шаг 3.
    ClientSession session_;
    SceneDesc sceneDesc_;      // сохранённое описание (host-режим отдаёт его серверу через session_)
    BuildingConfig config_;    // параметры/тексты типов зданий (из конфига)
    Grid grid_;                // строительная сетка (из описания сцены; та же, что у сервера)
    uint32_t selectedId_ = 0;  // id выделенной кликом сущности (0 = нет)
    uint8_t localTeam_ = 0;    // команда своего героя (из снапшота) — для ресурса per-team
    float localHp_ = 1.0f;      // hp своего героя (>0 = жив); из снапшота (вне сессии — «жив»)
    float localMaxHp_ = 100.0f; // макс. hp героя (из конфига)
    float localRespawn_ = 0.0f; // отсчёт респауна при поверженном (из aux сущности)

    // (Боевая косметика/звук/вью-матрицы — в presentation_ выше; emitSound — метод WorldPresentation.)

    // Стройка: активный режим + выбранный тип + материалы призрака (валид/невалид).
    bool buildActive_ = false;
    EntityType buildType_ = EntityType::Tower;
    MaterialHandle ghostOkMat_ = 0, ghostBadMat_ = 0;
    // Снап-подсветка сетки (режим стройки): один плоский тайл < клетки, инстансится на все
    // клетки арены; зазоры между тайлами образуют линии сетки. Материалы по состоянию клетки.
    MeshHandle gridTileMesh_ = 0;
    MaterialHandle gridFreeMat_ = 0, gridBusyMat_ = 0;  // свободна / занята зданием
    // Призрак: клетка перед героем + мировой центр + валидность. Возвращает валидность.
    bool computeGhost(int& cx, int& cz, Vec3& center) const;
    bool cellOccupied(int cx, int cz) const;  // клетка занята зданием (для сетки и призрака)
    // (client_/server_/… переехали в session_; pending_/simClock_/buildingColliders_/collision_ и
    // applySnapshot/syncBuildingColliders/killerYaw — в clientWorld_.)
    std::vector<RemoteEntity> remoteEntities_;  // все чужие сущности (герои/здания/…); читаются render/пикингом
    float tickDt_ = kTickDt;             // длительность тика (единый шаг, из engine/net/Net.h)
};
