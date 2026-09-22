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

    // Выделенная защита (башня/ловушка) — для панели апгрейда/сноса. Все -1/0/false, если
    // выделено не то (не защита / чужое / нет ростера).
    bool selectedIsDefense() const;         // выделена башня или ловушка
    bool selectedMine() const;              // выделенное принадлежит моей команде
    const char* selectedDefenseName() const;// имя вида (или "")
    int selectedTier() const;               // тир 1..maxTier (или 0)
    int selectedMaxTier() const;            // maxTier вида (или 0)
    float selectedUpgradeCost() const;      // цена апгрейда (0 = нельзя/макс)
    bool selectedCanUpgrade() const;        // тир<max И хватает ресурса
    bool selectedDemolishable() const;      // своя башня/ловушка/генератор/хранилище
    float selectedRefund() const;           // сколько вернётся при сносе (оценка, как на сервере)
    void upgradeSelected();                 // послать запрос апгрейда выделенного
    void demolishSelected();                // послать запрос сноса выделенного

    // Палитра защиты (ростер towers.cfg) — для панели строительства.
    int defenseCount() const;               // число видов в ростере
    const char* defenseName(int kind) const;// имя вида (или "")
    int defenseCost(int kind) const;        // цена постройки тира 1
    int defenseEntityType(int kind) const;  // EntityType вида (Tower/Trap) или -1

    // Стройка (G3-B): выбор типа -> призрак перед героем на клетке сетки -> подтверждение.
    // Размещение авторитетно на сервере (клиент лишь шлёт запрос и рисует превью).
    void beginBuild(int type, int kind = 0);   // kind — вид защиты (Tower/Trap); иначе игнор
    void cancelBuild() { buildActive_ = false; }
    void confirmBuild();                       // отправить запрос постройки на клетку призрака
    bool buildMode() const { return buildActive_; }
    int buildType() const { return (int)buildType_; }
    int buildKind() const { return buildKind_; }
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
    int enemyCount() const;    // живых врагов в снапшотах (для HUD «врагов живо»)
    float matchTime() const { return matchClock_; }  // длительность текущего боя, сек (для итога)
    void resetMatchClock() { matchClock_ = 0.0f; prevPhase_ = 0; }  // при входе в бой
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

    // Выбор персонажа (экран входа в бой / Lobby).
    int rosterCount() const { return (int)chars_.size(); }
    const char* rosterName(int i) const {
        return (i >= 0 && i < (int)chars_.size()) ? chars_[i].name.c_str() : "";
    }
    // Статы героя для брифинга выбора (из characters.cfg; клиент бой не считает). Вне диапазона -> 0.
    float rosterHp(int i) const {
        return (i >= 0 && i < (int)chars_.size()) ? chars_[i].hp : 0.0f;
    }
    float rosterDamage(int i) const {
        return (i >= 0 && i < (int)chars_.size()) ? chars_[i].damage : 0.0f;
    }
    float rosterRange(int i) const {
        return (i >= 0 && i < (int)chars_.size()) ? chars_[i].range : 0.0f;
    }
    float rosterSpeed(int i) const {
        return (i >= 0 && i < (int)chars_.size()) ? chars_[i].speed : 0.0f;
    }
    bool rosterRanged(int i) const {
        return (i >= 0 && i < (int)chars_.size()) && chars_[i].ranged;
    }
    int selectedCharacter() const { return localCharIndex_; }
    void selectCharacter(int i);  // локальный персонаж + уведомить сервер (для рендера чужими)

    // Брифинг текущей (уже построенной) сцены для экрана входа в бой. Считается из
    // сохранённого sceneDesc_ (без GPU-сборки): цель, HP ядра, параметры волн, режим.
    struct SceneBriefing {
        bool hasCore = false;      // в сцене есть ядро (цель обороны)
        float coreHp = 0.0f;       // суммарное HP ядер (0 если ядра нет)
        bool infiniteWaves = false; // есть спавнер с бесконечными волнами (waveSize>0)
        int waveBase = 0;          // суммарный размер первой волны по всем спавнерам
        int waveGrow = 0;          // суммарный прирост размера за волну
        int spawnerCount = 0;      // число спавнеров врагов
        bool pvp = false;          // в сцене есть враждующие команды (иначе PvE соло/кооп)
    };
    SceneBriefing sceneBriefing() const;

    // Мини-карта сцены (top-down, плоскость XZ) для брифинга: границы мира + стены/препятствия
    // (AABB) + точки интереса. Плоские данные из sceneDesc_ (без рендера) — UI проецирует и рисует.
    // Пол исключён (колайдеры, чей верх у земли); границы считаются по нарисованным фигурам.
    struct Minimap {
        bool valid = false;
        float minX = 0.0f, minZ = 0.0f, maxX = 0.0f, maxZ = 0.0f;  // мировые границы XZ
        struct Wall { float cx, cz, hx, hz; };  // AABB стены/препятствия (центр + полуразмеры)
        enum class Poi { Core, Spawner, HeroSpawn };
        struct Mark { float x, z; Poi kind; };
        std::vector<Wall> walls;
        std::vector<Mark> marks;
    };
    Minimap sceneMinimap() const;

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

    // Рендер вне боя (главный цикл выбирает путь по renderPath): 3D-превью выбранного
    // персонажа (экран входа в бой / Lobby) и пустой фон меню (мир не показываем).
    RenderFrame renderCharacterPreview(float alpha, float aspect, float renderDt);
    RenderFrame renderMenuBackdrop(float aspect);

    // Рендер уровня для редактора сцен: вся СТАТИЧНАЯ геометрия (объекты + здания из sceneDesc_
    // по их визуалам) с ПРОИЗВОЛЬНОЙ камерой (view/proj/eye задаёт редактор — орбита). Без сети,
    // симуляции, героя и интерполяции. Свет/тени/туман — из сцены. Только чтение (const).
    RenderFrame renderEditor(const Mat4& view, const Mat4& proj, const Vec3& eye) const;

    // Undo/redo редактора: заменить описание сцены целиком и пересобрать GPU-ресурсы (объекты/
    // здания/спавны/свет) из него. Сессия/физика не трогаются (редактор их не использует).
    void editorReloadFromDesc(const SceneDesc& desc, Renderer& renderer, AssetSource& assets);

    // --- Пикинг/трансформ для редактора сцен ---
    // Три категории: объекты (specIndex в doc.objects), здания (индекс в sceneDesc_.buildings ==
    // doc.buildings), точки спавна (индекс в sceneDesc_.spawns == doc.spawns). Пик-функции дают
    // дистанцию t луча -> редактор выбирает глобально ближайшую категорию. Луч мировой.
    bool editorPickObject(const Vec3& ro, const Vec3& rd, int& idx, float& t) const;
    bool editorPickBuilding(const Vec3& ro, const Vec3& rd, int& idx, float& t) const;
    bool editorPickSpawn(const Vec3& ro, const Vec3& rd, int& idx, float& t) const;
    bool editorObjectMatrix(int specIndex, Mat4& out) const;  // модельная матрица (для ImGuizmo)
    bool editorGetTransform(int specIndex, Vec3& pos, Vec3& rot, Vec3& scale) const;
    void editorSetTransform(int specIndex, const Vec3& pos, const Vec3& rot, const Vec3& scale);
    bool editorWorldAABB(int specIndex, Vec3& mn, Vec3& mx) const;  // для подсветки выделения
    // Добавить объект-модель (glTF) в КОНЕЦ списка. specIndex — его индекс в doc.objects редактора
    // (append -> последний). Возвращает specIndex или -1, если модель не загрузилась.
    int editorAddObjectModel(Renderer& renderer, AssetSource& assets, const std::string& model,
                             const std::string& tex, ShaderType shader, const Vec3& pos,
                             int specIndex);
    // Дублировать существующий объект (модель ИЛИ примитив box/sphere): клонирует живой GameObject
    // с его хендлами меша/материала/альбедо (перезагрузка не нужна), ставит новую позицию и
    // specIndex. Работает и для примитивов, у которых нет model (mesh/material — по имени из .scene).
    // Возвращает newSpecIndex или -1, если исходный объект не найден.
    int editorDuplicateObject(int srcSpecIndex, const Vec3& pos, int newSpecIndex);
    // Удалить объект по specIndex: убрать его GameObject + сдвинуть большие индексы вниз (в такт
    // erase в doc.objects редактора, чтобы specIndex == индексу в doc сохранялся).
    void editorRemoveObject(int specIndex);
    // Пересобрать материал объекта под новый shader/тинт (текстура сохраняется). Для model-объектов.
    void editorSetObjectMaterial(Renderer& renderer, int specIndex, ShaderType shader,
                                 const Vec3& color);

    // --- Здания (sceneDesc_.buildings; редактор держит doc.buildings 1:1) ---
    int editorBuildingCount() const { return (int)sceneDesc_.buildings.size(); }
    // Тип (EntityType), команда и позиция здания i. false — вне диапазона.
    bool editorBuildingInfo(int i, int& type, int& team, Vec3& pos) const;
    void editorSetBuildingPos(int i, const Vec3& pos);
    void editorSetBuildingTeam(int i, int team);
    bool editorBuildingWorldAABB(int i, Vec3& mn, Vec3& mx) const;
    int editorAddBuilding(int type, const Vec3& pos, int team);  // -> индекс нового
    void editorRemoveBuilding(int i);

    // --- Точки спавна (sceneDesc_.spawns; редактор держит doc.spawns 1:1) ---
    int editorSpawnCount() const { return (int)sceneDesc_.spawns.size(); }
    bool editorSpawnInfo(int i, int& team, Vec3& pos) const;
    void editorSetSpawnPos(int i, const Vec3& pos);
    void editorSetSpawnTeam(int i, int team);
    bool editorSpawnWorldAABB(int i, Vec3& mn, Vec3& mx) const;
    int editorAddSpawn(const Vec3& pos, int team);  // -> индекс нового
    void editorRemoveSpawn(int i);

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
    MaterialHandle projMat_ = 0;     // материал болта по умолчанию (снаряд героя): тёпло-жёлтый
    MeshHandle editorMarkerMesh_ = 0;      // маркер точки спавна (редактор)
    static constexpr int kEditorTeamColors = 4;  // 0=нейтр/кооп, 1/2/3 — стороны
    MaterialHandle editorMarkerMat_[kEditorTeamColors] = {0, 0, 0, 0};  // цвет маркера по команде

    // Виды башен/ловушек (towers.cfg): тинт по виду (kind) + плоская плита ловушки + цвет болта.
    // Индекс = kind (порядок ростера sceneDesc_.towerTypes). Заполняются в createGpuResources.
    TextureHandle towerTex_ = 0;               // атлас модели башни (для тинтованных материалов)
    std::vector<MaterialHandle> towerKindMat_; // материал башни/ловушки по виду (тинт из towers.cfg)
    std::vector<MaterialHandle> projKindMat_;  // материал болта по виду (цвет из towers.cfg)
    MeshHandle trapMesh_ = 0;                  // плита напольной ловушки (низкий широкий куб)

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
    int buildKind_ = 0;        // вид защиты (индекс в towers.cfg) при стройке Tower/Trap
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
    // Часы боя (для экрана итога): тикают в render(), пока фаза Playing; замерзают на исходе;
    // сбрасываются при (пере)старте матча и при входе в бой (resetMatchClock).
    float matchClock_ = 0.0f;
    int prevPhase_ = 0;                  // фаза в прошлом кадре (детект старт/рестарт для часов)
};
