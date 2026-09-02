#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include "engine/assets/AssetSource.h"
#include "engine/audio/Audio.h"  // SoundId/SoundEvent (портативный словарь; без аудио-движка)
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
struct CharacterDesc;  // game/CharacterRoster.h (ростер персонажей/мобов)

// Снапшот состояния во времени (для буфера интерполяции чужих игроков).
struct TimedState {
    double t = 0.0;  // время приёма (по часам симуляции), сек
    Vec3 pos{0.0f, 0.0f, 0.0f};
    float yaw = 0.0f;
    float anim = 0.0f;
    float attack = 0.0f;  // остаток времени атаки (для рендера каста чужих)
};

// Чужая сущность (не свой герой): любой тип из снапшота — герой, генератор,
// хранилище, враг, … Рендерим по типу; подвижные интерполируем через буфер.
struct RemoteEntity {
    uint32_t id = 0;
    uint8_t type = 0;   // EntityType
    uint8_t team = 0;
    uint8_t charType = 0;  // индекс персонажа в ростере (какой моделью рисовать чужого героя)
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
    bool netConnected() const { return client_.connected(); }
    bool netHost() const { return host_; }
    // Диагностика соединения (для HUD/панели): пинг + производные от статуса флаги.
    int netPingMs() const { return client_.pingMs(); }        // RTT до сервера, мс (-1 нет)
    bool netConnecting() const { return client_.status() == NetStatus::Connecting; }
    bool netConnectionLost() const { return client_.status() == NetStatus::Lost; }
    const char* netServerAddress() const { return serverIp_; }  // цель join (для UI)
    int netServerPort() const { return (int)serverPort_; }
    int netReconnectAttempts() const { return reconnectAttempts_; }  // попыток реконнекта
    void netRetryNow() { reconnectTimer_ = 0.0f; }  // форсировать реконнект сейчас
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

    // Читаемость боя: worldspace HP-бары + всплывающие числа урона. Scene (game/) только
    // ПРОИЗВОДИТ данные (диф hp из снапшотов, без ImGui); рисует GameUi (engine/render),
    // проецируя мировые точки через view/proj. Чистая клиентская косметика — протокол не трогает.
    struct CombatMarker { Vec3 pos; float hpFrac; Vec3 color; };       // полоска HP над сущностью
    struct DamageNumber { Vec3 pos; float amount; float age; Vec3 color; };  // всплывающее число
    struct ImpactSpark { Vec3 pos; float age; float maxAge; uint32_t seed; };  // вспышка-искры в точке хита
    struct BuildPoof { Vec3 pos; float age; };  // «пуф» при постройке (расходящееся кольцо)
    const std::vector<CombatMarker>& combatMarkers() const { return markers_; }
    const std::vector<DamageNumber>& damageNumbers() const { return damageNumbers_; }
    const std::vector<ImpactSpark>& impactSparks() const { return sparks_; }
    const std::vector<BuildPoof>& buildPoofs() const { return poofs_; }
    // Мировая позиция выделенного кликом здания (ринг выделения в оверлее). false = нет выделения.
    bool selectedWorldPos(Vec3& out) const;
    const Mat4& viewMatrix() const { return lastView_; }  // матрицы последнего кадра боя
    const Mat4& projMatrix() const { return lastProj_; }

    // Звуковые события за кадр: Scene накапливает из снапшотов/действий, платформа сливает и
    // проигрывает через Audio, затем чистит. Тут только словарь SoundId — без аудио-движка.
    const std::vector<SoundEvent>& sounds() const { return sounds_; }
    void clearSounds() { sounds_.clear(); }

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

private:
    FollowCamera camera_;
    std::vector<GameObject> objects_;

    // Манифест доступных сцен (config/scenes.cfg): путь + отображаемое имя. Меню
    // перечисляет их и просит платформу перезагрузить мир выбранной.
    struct SceneEntry { std::string path, name; };
    std::vector<SceneEntry> sceneList_;
    std::string currentScenePath_;     // путь сцены, которой построен этот Scene
    void loadSceneManifest(AssetSource& assets, const char* path);  // config/scenes.cfg -> sceneList_

    // Реестр выбираемых персонажей (грузится в build из config/characters.cfg). ВСЕ модели
    // грузятся один раз -> мгновенное переключение и рендер чужих их моделями без утечек GPU
    // (у Renderer нет удаления мешей/текстур). Индексы клипов резолвятся ПО ИМЕНИ (findAnimation).
    struct PlayerModel {
        std::string id, name;
        SkinnedModel model;          // данные скелета/анимаций
        SkinnedHandle mesh = 0;      // GPU-меш (клиентский ресурс)
        TextureHandle tex = 0;
        float scale = 1.0f, yawOffset = 0.0f;
        int idleClip = 0, walkClip = 1, runClip = 2, attackClip = -1, deathClip = -1;
        float attackClipDur = 0.0f;  // длительность клипа атаки, сек (масштаб под kAttackDuration)
        float deathClipDur = 0.0f;   // длительность клипа смерти, сек (для «трупа» моба)
        float hp = 0.0f;             // статы героя (для HUD/предсказания): 0 = дефолт
        float speed = 0.0f;          // скорость бега героя (client player_.maxSpeed для предсказания)
    };
    std::vector<PlayerModel> chars_;   // ростер героев; индекс = charType в снапшоте (сетевой контракт)
    std::vector<PlayerModel> mobs_;    // ростер мобов (config/enemies.cfg); индекс = enemy.charType
    int localCharIndex_ = 0;           // выбранный локальным игроком
    float previewSpin_ = 0.0f;         // накопленный угол вращения модели на экране выбора

    // «Труп» убитого моба: локальная косметика — проигрывает клип смерти на месте гибели и
    // убирается. Заводится, когда враг исчез из снапшота в фазе боя (см. applySnapshot).
    struct DyingMob {
        int charType = 0;
        Vec3 pos{0.0f, 0.0f, 0.0f};
        float yaw = 0.0f;
        float t = 0.0f;    // время с начала смерти
        float dur = 1.0f;  // длительность клипа смерти
    };
    std::vector<DyingMob> dyingMobs_;

    // Снаряды башен теперь СЕРВЕРНЫЕ сущности (EntityType::Projectile в снапшотах) — рисуем их
    // в remote-цикле этим мешем/материалом (болт). Клиентская симуляция снаряда убрана.
    MeshHandle projMesh_ = 0;        // меш болта (единичный куб, масштабируется в вытянутый)
    MaterialHandle projMat_ = 0;     // материал болта (Unlit, свечение)

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
    bool attackQueued_ = false;        // запрошена атака/каст (сбрасывается в fixedUpdate)
    float uiScale_ = 1.0f;
    Vec3 lightDir_{0.4f, 1.0f, 0.6f};  // направление НА свет (из файла сцены)
    bool shadowsEnabled_ = true;       // тени (directional shadow map)
    float shadowBias_ = 0.0025f;       // сдвиг глубины против self-shadow acne
    float shadowRadius_ = 14.0f;       // полуширина орто-коробки света (охват арены)
    Vec3 fogColor_{0.09f, 0.13f, 0.20f};  // цвет тумана (линейное пространство)
    float fogDensity_ = 0.014f;        // плотность экспоненциального тумана (0 = выкл)

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

    // Читаемость боя (клиентская косметика; протокол не трогает). Максимума hp в снапшоте нет —
    // берём наблюдаемый максимум (сущности спавнятся на полном hp → первое/наибольшее = max).
    std::vector<CombatMarker> markers_;              // пересобирается каждый render() из интерп. позиций
    std::vector<DamageNumber> damageNumbers_;        // всплывающие числа урона (живут ~kDmgLife, копятся в applySnapshot)
    std::unordered_map<uint32_t, float> maxHpSeen_;  // id -> наблюдаемый максимум hp (доля бара)
    std::unordered_map<uint32_t, float> flash_;      // id -> остаток hit-flash/scale-punch, сек
    float localFlash_ = 0.0f;                        // hit-flash/punch своего героя
    Mat4 lastView_, lastProj_;                       // матрицы последнего кадра боя (проекция маркеров в GameUi)

    // Джус/impact (клиентская косметика): искры в точке хита + тряска камеры на крупный урон.
    std::vector<ImpactSpark> sparks_;  // искры-вспышки (живут ~kSparkLife, рисует GameUi)
    std::vector<BuildPoof> poofs_;     // «пуфы» постройки (живут ~kPoofLife, рисует GameUi)
    uint32_t sparkSeed_ = 0x9e3779b9;  // накопитель для псевдослучайных направлений искр
    float shakeTime_ = 0.0f;           // остаток тряски камеры, сек
    float shakeAmp_ = 0.0f;            // пиковая амплитуда тряски (world units)

    // Звук (клиентская косметика): очередь событий кадра + фронт фазы матча.
    std::vector<SoundEvent> sounds_;   // накапливается в applySnapshot/действиях; см. sounds()
    uint8_t prevPhase_ = 0;            // прошлый GamePhase — звук победы/поражения по фронту
    void emitSound(SoundId id);        // добавить звук с кэпом повторов за кадр (антиспам)

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
    bool host_ = false;
    // Авто-реконнект для join-сессии (host к 127.0.0.1 не переподключаем). serverIp_
    // запоминается в joinGame; при статусе Lost повторяем connect раз в kReconnectPeriod.
    char serverIp_[64] = {0};
    uint16_t serverPort_ = kNetPort;   // порт join-сессии (для реконнекта; настраивается в UI)
    int reconnectAttempts_ = 0;        // счётчик попыток реконнекта (для UI)
    bool wantReconnect_ = false;
    float reconnectTimer_ = 0.0f;
    uint32_t inputSeq_ = 0;
    std::vector<RemoteEntity> remoteEntities_;  // все чужие сущности (герои/здания/…)
    std::vector<PendingInput> pending_;  // неподтверждённые вводы (для реплея)
    double simClock_ = 0.0;              // часы симуляции (сек)
    float tickDt_ = kTickDt;             // длительность тика (единый шаг, из engine/net/Net.h)

    // Футпринт-коллайдеры зданий на клиенте (id сущности -> ColliderBoxId). Сервер ставит боксы
    // блокирующим зданиям (GameWorld::attachFootprint), и предсказание героя должно видеть ту же
    // геометрию — иначе герой прошёл бы сквозь здание локально, а сервер вытолкнул = rubber-band.
    // Реконсилируется против remoteEntities_ по снапшотам (см. syncBuildingColliders).
    std::unordered_map<uint32_t, uint32_t> buildingColliders_;

    void applySnapshot();
    void syncBuildingColliders();  // добавить/убрать боксы зданий под текущий remoteEntities_
    // Yaw «лицом к убийце» для корпуса моба: ближайший герой/башня (они и бьют/стреляют мобов).
    // death-клип бросает НАЗАД от facing → доворот к источнику = бросок ОТ него. false = кандидата нет.
    bool killerYaw(const Vec3& mobPos, uint32_t mobId, float& outYaw) const;
};
