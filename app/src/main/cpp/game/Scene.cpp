#include "game/Scene.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <unordered_map>

#include "engine/assets/AssetSource.h"
#include "engine/assets/Assets.h"
#include "engine/physics/CollisionWorld.h"
#include "game/CharacterRoster.h"
#include "game/Grid.h"
#include "engine/core/Log.h"
#include "engine/core/Renderer.h"
#include "game/SceneLoader.h"

namespace {
// Запечь статичную OBJ-модель под generic-рендер (он только translation): центрируем по XZ,
// ставим низ на y=0, масштабируем и поворачиваем вокруг Y — прямо в вершины (позиции+нормали).
void bakeStaticMesh(MeshData& m, float scale, float yawDeg) {
    if (m.vertices.empty()) return;
    float mnx = 1e30f, mxx = -1e30f, mny = 1e30f, mnz = 1e30f, mxz = -1e30f;
    for (const Vertex& v : m.vertices) {
        mnx = std::fmin(mnx, v.px); mxx = std::fmax(mxx, v.px);
        mny = std::fmin(mny, v.py);
        mnz = std::fmin(mnz, v.pz); mxz = std::fmax(mxz, v.pz);
    }
    float cx = (mnx + mxx) * 0.5f, cz = (mnz + mxz) * 0.5f;
    float r = yawDeg * 3.14159265f / 180.0f;
    float c = std::cos(r), s = std::sin(r);
    for (Vertex& v : m.vertices) {
        float x = (v.px - cx) * scale, y = (v.py - mny) * scale, z = (v.pz - cz) * scale;
        v.px = x * c + z * s; v.py = y; v.pz = -x * s + z * c;
        float nx = v.nx, nz = v.nz;
        v.nx = nx * c + nz * s; v.nz = -nx * s + nz * c;
    }
}
}  // namespace

Scene::Scene() = default;
Scene::~Scene() = default;

// Манифест сцен config/scenes.cfg: строка = "<путь> <имя с пробелами...>". Первый токен —
// путь, остаток строки (после пробелов) — отображаемое имя. Комментарии с '#'. Отсутствие
// файла не критично (список будет пуст, меню просто не покажет выбор).
void Scene::loadSceneManifest(AssetSource& assets, const char* path) {
    sceneList_.clear();
    std::vector<uint8_t> bytes;
    if (!assets.read(path, bytes)) {
        LOGW("scenes: манифест не найден: %s (выбор сцен недоступен)", path);
        return;
    }
    std::string text(bytes.begin(), bytes.end());
    std::istringstream in(text);
    std::string raw;
    while (std::getline(in, raw)) {
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();  // CRLF
        size_t hash = raw.find('#');
        if (hash != std::string::npos) raw.erase(hash);
        std::istringstream ls(raw);
        std::string p;
        if (!(ls >> p)) continue;                 // пустая строка
        std::string name;
        std::getline(ls, name);                   // остаток строки — имя
        size_t s = name.find_first_not_of(" \t");
        size_t e = name.find_last_not_of(" \t");
        name = (s == std::string::npos) ? p : name.substr(s, e - s + 1);
        sceneList_.push_back({p, name});
    }
    LOGI("scenes: в манифесте %d сцен", (int)sceneList_.size());
}

void Scene::build(Renderer& renderer, AssetSource& assets, const char* scenePath) {
    objects_.clear();
    collision_ = std::make_unique<CollisionWorld>();  // свежий мир коллизий на каждую сборку

    currentScenePath_ = scenePath ? scenePath : "";
    loadSceneManifest(assets, "config/scenes.cfg");  // список сцен для меню (не критично, если нет)

    SceneDesc desc;
    if (!loadSceneDesc(assets, scenePath, desc)) {
        LOGE("Не удалось загрузить сцену: %s", scenePath);
        return;  // пустая сцена — ошибки уже в логе
    }
    sceneDesc_ = desc;  // сохраняем: host-режим отдаст ту же геометрию своему серверу
    grid_ = desc.grid;  // строительная сетка из сцены (та же, что применит сервер)
    // Конфиг зданий (параметры + тексты панели). Параметры применяем к зданиям сцены —
    // единый источник настроек: сцена размещает, конфиг задаёт rate/cap/…
    loadBuildingConfig(assets, "config/buildings.cfg", config_);
    applyBuildingConfig(sceneDesc_, config_);
    localMaxHp_ = config_.get(EntityType::Hero).hp;  // для HUD-бара героя
    if (localMaxHp_ <= 0.0f) localMaxHp_ = 100.0f;

    // Свет и камера — прямо из описания.
    lightDir_ = desc.lightDir;
    camera_.distance = desc.camera.distance;
    camera_.pitch = desc.camera.pitch;
    camera_.lookHeight = desc.camera.lookHeight;
    camera_.fovY = desc.camera.fovY;
    camera_.nearZ = desc.camera.nearZ;
    camera_.farZ = desc.camera.farZ;

    // --- Текстуры (имя -> GPU-handle) ---
    std::unordered_map<std::string, TextureHandle> texMap;
    for (const TextureSpec& ts : desc.textures) {
        TextureHandle h = 0;
        if (ts.kind == TextureSpec::Checker) {
            h = renderer.createTexture(makeCheckerboard((uint32_t)ts.size, (uint32_t)ts.cells));
        } else if (ts.kind == TextureSpec::Bump) {
            h = renderer.createTexture(makeBumpNormal((uint32_t)ts.size, (uint32_t)ts.cells));
        } else {
            TextureData img;
            if (loadImageAsset(assets, ts.path.c_str(), img)) h = renderer.createTexture(img);
            else LOGW("Текстура-картинка не найдена: %s (материал станет белым)", ts.path.c_str());
        }
        texMap[ts.name] = h;
    }
    // Разрешить ссылку на текстуру: имя объявленной ИЛИ путь к картинке ИЛИ пусто.
    auto resolveTex = [&](const std::string& ref) -> TextureHandle {
        if (ref.empty()) return 0;
        auto it = texMap.find(ref);
        if (it != texMap.end()) return it->second;
        TextureData img;
        if (loadImageAsset(assets, ref.c_str(), img)) return renderer.createTexture(img);
        LOGW("Текстура не найдена: %s", ref.c_str());
        return 0;
    };

    // --- Материалы (имя -> handle) ---
    std::unordered_map<std::string, MaterialHandle> matMap;
    for (const MaterialSpec& ms : desc.materials) {
        MaterialDesc md;
        md.shader = ms.shader;
        md.baseColor = ms.color;
        md.albedo = resolveTex(ms.tex);
        md.normal = resolveTex(ms.normal);
        matMap[ms.name] = renderer.createMaterial(md);
    }

    // --- Меши (имя -> handle) ---
    std::unordered_map<std::string, MeshHandle> meshMap;
    for (const MeshSpec& m : desc.meshes) {
        MeshHandle h = 0;
        switch (m.kind) {
            case MeshSpec::Plane:  h = renderer.createMesh(makePlane(m.a, m.b)); break;
            case MeshSpec::Cube:   h = renderer.createMesh(makeCube(m.a)); break;
            case MeshSpec::Sphere: h = renderer.createMesh(makeSphere(m.a, m.stacks, m.slices)); break;
        }
        meshMap[m.name] = h;
    }

    auto meshH = [&](const std::string& n) -> MeshHandle {
        auto it = meshMap.find(n);
        if (it == meshMap.end()) { LOGW("Неизвестный меш в объекте: %s", n.c_str()); return 0; }
        return it->second;
    };
    auto matH = [&](const std::string& n) -> MaterialHandle {
        auto it = matMap.find(n);
        if (it == matMap.end()) { LOGW("Неизвестный материал в объекте: %s", n.c_str()); return 0; }
        return it->second;
    };

    // --- Объекты (обычные и кольцевые) ---
    const float pi = 3.14159265358979323846f;
    for (const ObjectSpec& os : desc.objects) {
        MeshHandle mh = meshH(os.mesh);
        MaterialHandle mah = matH(os.material);
        if (os.ring) {
            for (int k = 0; k < os.ringCount; ++k) {
                float a = 2.0f * pi * (float)k / (float)(os.ringCount > 0 ? os.ringCount : 1);
                GameObject c;
                c.mesh = mh;
                c.material = mah;
                c.transform.position = {std::cos(a) * os.ringRadius, os.ringY, std::sin(a) * os.ringRadius};
                c.transform.rotation = os.rot;
                c.transform.scale = {os.scale, os.scale, os.scale};
                c.spin = os.spin;
                c.prevRotY = os.rot.y;
                objects_.push_back(c);
            }
        } else {
            GameObject o;
            o.mesh = mh;
            o.material = mah;
            o.transform.position = os.pos;
            o.transform.rotation = os.rot;
            o.transform.scale = {os.scale, os.scale, os.scale};
            o.spin = os.spin;
            o.prevRotY = os.rot.y;
            objects_.push_back(o);
        }
    }

    // --- Визуалы сущностей по типу: ОДНА таблица, заполняется ИЗ КОНФИГА (config/buildings.cfg:
    // shape/material/yOffset/pickRadius). Рендер/пикинг/призрак читают её через visual(); yOffset
    // не дублируется. Hero и типы без shape в таблицу не попадают (Hero — скиннинг-лиса).
    std::unordered_map<std::string, TextureHandle> modelTexCache;  // атлас грузим 1 раз на путь
    for (int t = 0; t < kEntityVisualCount; ++t) {
        const BuildingInfo& bi = config_.byType[t];
        EntityVisual& v = visuals_[t];

        // Приоритет — статичная OBJ-модель (KayKit dungeon и т.п.). Не загрузилась -> откат на shape.
        if (!bi.model.empty()) {
            MeshData mesh;
            if (loadObjAsset(assets, bi.model.c_str(), mesh)) {
                bakeStaticMesh(mesh, bi.modelScale, bi.modelYawDeg);
                v.mesh = renderer.createMesh(mesh);
                TextureHandle tex = 0;
                if (!bi.modelTex.empty()) {
                    auto it = modelTexCache.find(bi.modelTex);
                    if (it != modelTexCache.end()) {
                        tex = it->second;
                    } else {
                        TextureData td;
                        if (loadImageAsset(assets, bi.modelTex.c_str(), td)) tex = renderer.createTexture(td);
                        modelTexCache[bi.modelTex] = tex;
                    }
                }
                MaterialDesc md;
                md.shader = bi.shader;
                md.baseColor = {1.0f, 1.0f, 1.0f};  // цвет из текстуры
                md.albedo = tex;
                v.material = renderer.createMaterial(md);
                v.yOffset = bi.yOffset;
                v.pickRadius = bi.pickRadius;
                v.pickable = true;
                v.building = (EntityType)t != EntityType::Enemy;
                LOGI("Здание тип %d: модель %s (%d верш.)", t, bi.model.c_str(), (int)mesh.vertices.size());
                continue;
            }
            LOGW("Здание тип %d: модель %s не загрузилась — откат на shape", t, bi.model.c_str());
        }

        if (bi.shape == MeshShape::None) continue;  // визуал в конфиге не задан
        v.mesh = (bi.shape == MeshShape::Sphere)
                     ? renderer.createMesh(makeSphere(bi.shapeSize, bi.shapeStacks, bi.shapeSlices))
                     : renderer.createMesh(makeCube(bi.shapeSize));
        MaterialDesc md;
        md.shader = bi.shader;
        md.baseColor = bi.color;
        v.material = renderer.createMaterial(md);
        v.yOffset = bi.yOffset;
        v.pickRadius = bi.pickRadius;
        v.pickable = true;                                // всё с визуалом пикается (Hero — нет)
        v.building = (EntityType)t != EntityType::Enemy;  // враг не занимает клетку сетки
    }
    // Материалы призрака размещения (Unlit): зелёный = можно, красный = нельзя.
    {
        MaterialDesc md;
        md.shader = ShaderType::Unlit;
        md.baseColor = {0.30f, 0.90f, 0.40f};
        ghostOkMat_ = renderer.createMaterial(md);
    }
    {
        MaterialDesc md;
        md.shader = ShaderType::Unlit;
        md.baseColor = {0.95f, 0.35f, 0.30f};
        ghostBadMat_ = renderer.createMaterial(md);
    }
    // Снаряд башни: единичный куб (масштабируется в вытянутый болт) + свечение (Unlit).
    projMesh_ = renderer.createMesh(makeCube(1.0f));
    {
        MaterialDesc md;
        md.shader = ShaderType::Unlit;
        md.baseColor = {1.0f, 0.85f, 0.30f};  // тёпло-жёлтый болт
        projMat_ = renderer.createMaterial(md);
    }
    // Тайл подсветки сетки: плоскость чуть меньше клетки — зазоры дают линии сетки. Один меш
    // инстансится на все клетки (батч по mesh+material), поэтому цвет — в материале, не в тайле.
    gridTileMesh_ = renderer.createMesh(makePlane(grid_.cell * 0.9f));
    {
        MaterialDesc md;
        md.shader = ShaderType::Unlit;
        md.baseColor = {0.16f, 0.26f, 0.22f};  // свободная клетка — приглушённый нейтральный
        gridFreeMat_ = renderer.createMaterial(md);
    }
    {
        MaterialDesc md;
        md.shader = ShaderType::Unlit;
        md.baseColor = {0.42f, 0.14f, 0.12f};  // занятая клетка — приглушённый красный
        gridBusyMat_ = renderer.createMaterial(md);
    }

    // --- Статичные коллайдеры физики (из описания сцены) ---
    for (const ColliderSpec& cs : desc.colliders) {
        collision_->addBox(cs.center, cs.half);
    }
    collision_->finalize();  // оптимизация broad-phase после всей статики
    LOGI("Физика: %d статичных коллайдеров", (int)desc.colliders.size());

    // --- Управляемый персонаж: ростер выбираемых моделей (выбор персонажа) ---
    if (desc.player.present) {
        // Данные ростера героев. Если конфиг не прочитан — один вход из player-директивы сцены
        // (обратная совместимость: играбельно и без characters.cfg).
        std::vector<CharacterDesc> roster;
        if (!loadCharacterRoster(assets, "config/characters.cfg", roster)) {
            CharacterDesc c;
            c.id = c.name = "player";
            c.model = desc.player.model;
            c.scale = desc.player.scale;
            c.yawOffset = desc.player.yawOffset;
            c.hide = desc.player.hideNodes;
            roster.push_back(std::move(c));
        }
        loadRosterModels(renderer, assets, roster, chars_);
        sceneDesc_.heroTypes = roster;  // статы героев (hp/speed) -> локальному серверу при hostGame

        // Персонаж по умолчанию — совпавший с player.model из сцены, иначе первый.
        int def = 0;
        for (size_t i = 0; i < roster.size(); ++i) {
            if (roster[i].model == desc.player.model) { def = (int)i; break; }
        }
        selectCharacter(def);

        // Ростер мобов (враги спавнеров). Нет файла -> mobs_ пуст -> враги рисуются
        // generic-мешем из visuals_ (обратная совместимость).
        std::vector<CharacterDesc> mobRoster;
        if (loadCharacterRoster(assets, "config/enemies.cfg", mobRoster)) {
            loadRosterModels(renderer, assets, mobRoster, mobs_);
            sceneDesc_.enemyTypes = mobRoster;  // статы типов мобов -> локальному серверу при hostGame
        }

        // Позиция и кинематический контроллер — независимо от загрузки модели (это игра).
        player_.position = desc.player.pos;
        player_.collider = collision_->addCharacter(desc.player.pos,
                                                    desc.player.colliderRadius,
                                                    desc.player.colliderCylHalf);
        player_.snapshot();  // prev = curr, чтобы первый кадр не «прыгнул»
    }
}

// Загрузить модели ростера в GPU-реестр (герои/мобы). Слот заводим ВСЕГДА (даже при ошибке —
// mesh=0), чтобы индекс совпадал с сетевым charType на всех клиентах. Клипы — по имени.
void Scene::loadRosterModels(Renderer& renderer, AssetSource& assets,
                             const std::vector<CharacterDesc>& roster,
                             std::vector<PlayerModel>& out) {
    for (const CharacterDesc& c : roster) {
        PlayerModel pm;
        pm.id = c.id;
        pm.name = c.name;
        pm.scale = c.scale;
        pm.yawOffset = c.yawOffset;
        pm.hp = c.hp;          // статы героя (для HUD/предсказания; у мобов не используются)
        pm.speed = c.speed;
        const std::vector<std::string>* hide = c.hide.empty() ? nullptr : &c.hide;
        if (loadGltfModel(assets, c.model.c_str(), pm.model, hide)) {
            pm.mesh = renderer.createSkinnedMesh(pm.model);
            if (pm.model.hasTexture) pm.tex = renderer.createTexture(pm.model.baseColor);
            int an = (int)pm.model.animations.size();
            pm.idleClip = pm.model.findAnimation({"idle", "survey"}, 0);
            pm.walkClip = pm.model.findAnimation({"walk"}, an > 1 ? 1 : pm.idleClip);
            pm.runClip  = pm.model.findAnimation({"run", "sprint"}, an > 2 ? 2 : pm.walkClip);
            // Клип атаки: сперва имя из ростера, затем keyword-поиск (маг: Spellcast_Shoot).
            pm.attackClip = pm.model.findAnimation(
                {c.attackClip, "spellcast_shoot", "spellcasting", "spellcast", "attack"}, -1);
            pm.attackClipDur = (pm.attackClip >= 0 && pm.attackClip < an)
                                   ? pm.model.animations[pm.attackClip].duration : 0.0f;
            // Клип смерти («труп» моба): у скелетов — рассыпание в кости, иначе Death_A.
            pm.deathClip = pm.model.findAnimation({"death_c_skeletons", "death_a", "death"}, -1);
            pm.deathClipDur = (pm.deathClip >= 0 && pm.deathClip < an)
                                  ? pm.model.animations[pm.deathClip].duration : 0.0f;
            LOGI("Модель '%s': idle=%d walk=%d run=%d attack=%d death=%d (%d анимаций)",
                 c.id.c_str(), pm.idleClip, pm.walkClip, pm.runClip, pm.attackClip, pm.deathClip, an);
        } else {
            LOGW("Модель '%s' (%s) не загрузилась — слот останется пустым",
                 c.id.c_str(), c.model.c_str());
        }
        out.push_back(std::move(pm));  // всегда — индексы держим синхронно с charType
    }
}

SkinnedItem Scene::makeSkinnedItem(const std::vector<PlayerModel>& reg, int index, Vec3 pos,
                                   float yaw, float animParam, float animTime,
                                   float attackTime, int oneShotClip, float oneShotTime) const {
    SkinnedItem item;
    if (index < 0 || index >= (int)reg.size()) return item;  // нет такой модели
    const PlayerModel& pm = reg[index];
    if (pm.mesh == 0) return item;  // слот пуст (модель не загрузилась) — не рисуем

    item.mesh = pm.mesh;
    item.texture = pm.tex;
    item.color = (pm.tex != 0) ? Vec3{1.0f, 1.0f, 1.0f} : Vec3{0.85f, 0.5f, 0.25f};
    item.model = Mat4::translation(pos)
               * Mat4::rotationY(yaw + pm.yawOffset)
               * Mat4::scale({pm.scale, pm.scale, pm.scale});

    if (!pm.model.animations.empty()) {
        // Разовый клип (смерть) перекрывает всё: проигрываем в oneShotTime, без зацикливания.
        if (oneShotClip >= 0 && oneShotClip < (int)pm.model.animations.size()) {
            pm.model.sampleAnimation(oneShotClip, oneShotTime, item.joints);
        // Атака перекрывает локомоцию: проигрываем клип каста ОДИН раз до конца, растянув
        // его ровно на окно kAttackDuration (сервер не знает длину клипа — масштаб здесь).
        } else if (attackTime > 0.0f && pm.attackClip >= 0 && pm.attackClipDur > 0.0f) {
            float frac = 1.0f - attackTime / Character::kAttackDuration;  // 0..1 прогресс каста
            frac = frac < 0.0f ? 0.0f : (frac > 0.999f ? 0.999f : frac);  // без зацикливания
            pm.model.sampleAnimation(pm.attackClip, frac * pm.attackClipDur, item.joints);
        } else if (animParam <= 0.01f) {  // Клипы выбраны по имени при загрузке.
            pm.model.sampleAnimation(pm.idleClip, animTime, item.joints);
        } else if (animParam <= 1.0f) {
            pm.model.sampleBlend(pm.idleClip, animTime, pm.walkClip, animTime, animParam, item.joints);
        } else {
            pm.model.sampleBlend(pm.walkClip, animTime, pm.runClip, animTime, animParam - 1.0f, item.joints);
        }
    }
    return item;
}

void Scene::selectCharacter(int i) {
    if (chars_.empty()) { localCharIndex_ = 0; return; }
    if (i < 0) i = 0;
    if (i >= (int)chars_.size()) i = (int)chars_.size() - 1;
    localCharIndex_ = i;
    client_.setCharType((uint8_t)i);  // сервер положит в снапшот -> чужие нарисуют нашей моделью
    // Статы выбранного героя: скорость — в предсказание (должна совпадать с сервером, иначе
    // реконсиляция дёргала бы), максимум hp — для HUD (текущий hp авторитетно из снапшота).
    if (chars_[i].speed > 0.0f) player_.maxSpeed = chars_[i].speed;
    if (chars_[i].hp > 0.0f) localMaxHp_ = chars_[i].hp;
}

// Экран выбора: выбранный персонаж в origin, idle-анимация, медленное вращение. Мир НЕ рисуем —
// фон = чистый цвет очистки. Главный цикл зовёт это вместо render() в режиме CharacterSelect.
RenderFrame Scene::renderCharacterPreview(float alpha, float aspect, float renderDt) {
    (void)alpha;
    previewSpin_ += renderDt;  // накопленное время: и фаза idle, и угол вращения

    RenderFrame frame;
    Vec3 target{0.0f, 1.2f, 0.0f};   // ~середина роста гуманоида
    Vec3 eye{0.0f, 1.5f, 4.6f};      // камера спереди, чуть сверху
    frame.view = Mat4::lookAt(eye, target, Vec3{0.0f, 1.0f, 0.0f});
    frame.proj = Mat4::perspective(0.9f, aspect, 0.1f, 100.0f);
    frame.cameraPos = eye;
    frame.lightDir = normalize(lightDir_);
    frame.shadowsEnabled = shadowsEnabled_;
    frame.shadowBias = shadowBias_;
    frame.shadowRadius = shadowRadius_;
    frame.fogColor = fogColor_;
    frame.fogDensity = fogDensity_;

    float yaw = previewSpin_ * 0.6f;                    // медленный оборот
    frame.skinned.push_back(
        makeSkinnedItem(chars_, localCharIndex_, Vec3{0.0f, 0.0f, 0.0f}, yaw, 0.0f, previewSpin_, 0.0f));
    return frame;
}

// Фон меню (MainMenu/Loading): пустой кадр — мир не показываем, только цвет очистки.
RenderFrame Scene::renderMenuBackdrop(float aspect) {
    RenderFrame frame;
    Vec3 eye{0.0f, 2.0f, 6.0f}, target{0.0f, 1.0f, 0.0f};
    frame.view = Mat4::lookAt(eye, target, Vec3{0.0f, 1.0f, 0.0f});
    frame.proj = Mat4::perspective(0.9f, aspect, 0.1f, 100.0f);
    frame.cameraPos = eye;
    frame.lightDir = normalize(lightDir_);
    frame.shadowsEnabled = shadowsEnabled_;
    frame.shadowBias = shadowBias_;
    frame.shadowRadius = shadowRadius_;
    frame.fogColor = fogColor_;
    frame.fogDensity = fogDensity_;
    return frame;
}

void Scene::setUiScale(float s) {
    uiScale_ = s;
    joystick_.radius = 120.0f * s;  // джойстик крупнее на плотных экранах
}

void Scene::fixedUpdate(float dt) {
    // Декор: запоминаем прошлый угол и крутим (интерполяция при рендере).
    for (GameObject& obj : objects_) {
        obj.prevRotY = obj.transform.rotation.y;
        obj.transform.rotation.y += obj.spin * dt;
    }

    // Ввод -> команда (это и уйдёт на сервер в будущем). Направление считаем
    // относительно камеры: вверх на стике = "от камеры вперёд".
    // Источник ввода: тач-джойстик (телефон) либо внешняя ось (клавиатура на ПК).
    float ix, iy, mag;
    if (joystick_.active) {
        ix = joystick_.x;
        iy = joystick_.y;
        mag = joystick_.mag;
    } else {
        ix = extX_;
        iy = extY_;
        mag = std::sqrt(ix * ix + iy * iy);
        if (mag > 1.0f) { ix /= mag; iy /= mag; mag = 1.0f; }
    }

    InputCommand cmd;
    if (mag > 0.05f) {
        float cy = camera_.yaw;
        Vec3 fwd{std::sin(cy), 0.0f, std::cos(cy)};
        Vec3 right{-std::cos(cy), 0.0f, std::sin(cy)};  // экранный right = cross(fwd, up)
        Vec3 moveDir = normalize(fwd * iy + right * ix);
        cmd.moveX = moveDir.x;
        cmd.moveZ = moveDir.z;
    }
    cmd.faceMove = true;      // всегда доворачиваемся к направлению движения (без пятясь-назад)
    cmd.magnitude = mag;      // полная скорость во все стороны
    cmd.jump = jumpQueued_;                          // одноразовый прыжок
    jumpQueued_ = false;
    cmd.attack = attackQueued_;                      // одноразовая атака (каст)
    attackQueued_ = false;

    if (heroDead()) {  // повержен — ввод в ноль (сервер держит героя на месте до респауна)
        cmd.moveX = cmd.moveZ = 0.0f;
        cmd.magnitude = 0.0f;
        cmd.jump = false;
        cmd.attack = false;
    }

    tickDt_ = dt;

    // Отправляем ввод серверу и запоминаем его как неподтверждённый (для реплея).
    if (client_.connected()) {
        cmd.seq = ++inputSeq_;
        client_.sendInput(cmd);
        pending_.push_back({cmd, dt});
    }

    player_.snapshot();                        // зафиксировать прошлое для интерполяции
    player_.simulate(dt, cmd, collision_.get());  // локальное предсказание (с коллизиями)
    player_.animTime += dt;  // фаза анимации — ровно 1 раз за тик (не в simulate: реплей
                             // реконсиляции зовёт simulate многократно и ускорял бы её)
    if (player_.attackTime > 0.0f) {  // отсчёт каста — тоже 1 раз/тик (триггер — в simulate)
        player_.attackTime -= dt;
        if (player_.attackTime < 0.0f) player_.attackTime = 0.0f;
    }

    // Сервер (если хостим) — тем же кодом симуляции, затем рассылка снапшотов.
    if (host_) {
        server_.poll();
        server_.tick(dt);
    }
    // Приём: снапшоты -> реконсиляция своего аватара + буфер чужих.
    client_.poll();
    if (client_.consumeSnapshot()) {
        applySnapshot();
    }

    // Реакция на обрыв. Чужие сущности застыли бы на последнем снапшоте — чистим их
    // (и неподтверждённые вводы), чтобы на экране не висели «призраки». Для join-сессии
    // раз в kReconnectPeriod пробуем переподключиться; host к 127.0.0.1 не трогаем.
    if (client_.status() == NetStatus::Lost) {
        if (!remoteEntities_.empty()) remoteEntities_.clear();
        if (!dyingMobs_.empty()) dyingMobs_.clear();
        if (!pending_.empty()) pending_.clear();
        if (wantReconnect_) {
            constexpr float kReconnectPeriod = 2.0f;  // сек между попытками
            reconnectTimer_ -= dt;
            if (reconnectTimer_ <= 0.0f) {
                reconnectTimer_ = kReconnectPeriod;
                LOGI("Scene: переподключение к %s", serverIp_);
                client_.connect(serverIp_, kNetPort);  // connect() сам сбросит клиент
                inputSeq_ = 0;
            }
        }
    }

    simClock_ += dt;
}

void Scene::applySnapshot() {
    uint32_t myId = client_.myId();
    if (myId == 0) return;  // ждём Welcome, иначе примем себя за чужого

    const std::vector<EntityState>& states = client_.states();
    const uint32_t ack = client_.ackSeq();

    for (const EntityState& s : states) {
        if (s.id == myId) {
            // Reconciliation: ставим авторитетное состояние сервера и ПЕРЕИГРЫВАЕМ
            // все вводы, которые сервер ещё не обработал (seq > ack).
            localTeam_ = s.team;  // своя команда — для ресурса/стройки per-team
            localHp_ = s.hp;      // ставки: hp своего героя (<=0 = повержен)
            localRespawn_ = s.aux;  // отсчёт респауна (сервер шлёт в aux при поверженном)
            player_.position = {s.x, s.y, s.z};
            player_.facingYaw = s.yaw;
            player_.speed01 = s.speed01;
            player_.animParam = s.animParam;
            player_.velocityY = s.velY;  // вертикаль тоже сбрасываем на серверную —
            // иначе реплей считает прыжок от чужой скорости и он дёргается.
            player_.attackTime = s.attackT;  // авторитетный остаток каста (реплей переиграет триггер)
            // Синхронизируем контроллер с авторитетной позицией перед реплеем,
            // иначе collide-and-slide стартует от устаревшей внутренней позиции.
            if (collision_ && player_.collider != 0) {
                collision_->setCharacterPosition(player_.collider, player_.position);
            }

            size_t w = 0;  // выкидываем подтверждённые (seq <= ack)
            for (size_t i = 0; i < pending_.size(); ++i) {
                if (pending_[i].cmd.seq > ack) pending_[w++] = pending_[i];
            }
            pending_.resize(w);
            for (const PendingInput& p : pending_) {
                player_.simulate(p.dt, p.cmd, collision_.get());  // реплей поверх сервера
            }
            continue;
        }

        // Чужая сущность (герой/генератор/хранилище/…): в буфер интерполяции + тип.
        RemoteEntity* r = nullptr;
        for (auto& re : remoteEntities_) {
            if (re.id == s.id) { r = &re; break; }
        }
        if (r == nullptr) {
            RemoteEntity re;
            re.id = s.id;
            re.ch.position = {s.x, s.y, s.z};
            re.ch.facingYaw = s.yaw;
            remoteEntities_.push_back(re);
            r = &remoteEntities_.back();
        }
        r->type = s.type;
        r->team = s.team;
        r->charType = s.charType;  // какой моделью рисовать чужого героя (индекс ростера)
        r->aux = s.aux;  // ресурс в хранилище и т.п. (последнее значение)
        r->hp = s.hp;    // здоровье (ядро/враг)
        r->buffer.push_back({simClock_, {s.x, s.y, s.z}, s.yaw, s.animParam, s.attackT});
        // Ограничиваем историю (~1 сек), чтобы буфер не рос.
        while (r->buffer.size() > 2 && r->buffer[1].t < simClock_ - 1.0) {
            r->buffer.erase(r->buffer.begin());
        }
    }

    // Убрать исчезнувшие сущности (нет в текущем снапшоте). Моб, пропавший в фазе боя, —
    // это убитый враг: оставляем локальный «труп» с анимацией смерти на его месте.
    const bool playing = (client_.gamePhase() == (uint8_t)GamePhase::Playing);
    for (size_t i = 0; i < remoteEntities_.size();) {
        bool found = false;
        for (const EntityState& s : states) {
            if (s.id == remoteEntities_[i].id) { found = true; break; }
        }
        if (!found) {
            const RemoteEntity& re = remoteEntities_[i];
            if (playing && (EntityType)re.type == EntityType::Enemy && !mobs_.empty()) {
                int mi = (int)((uint32_t)re.charType % (uint32_t)mobs_.size());
                if (mobs_[mi].deathClip >= 0 && mobs_[mi].deathClipDur > 0.0f) {
                    Vec3 p = re.buffer.empty() ? re.ch.position : re.buffer.back().pos;
                    float yaw = re.buffer.empty() ? re.ch.facingYaw : re.buffer.back().yaw;
                    dyingMobs_.push_back({mi, p, yaw, 0.0f, mobs_[mi].deathClipDur});
                }
            }
            remoteEntities_.erase(remoteEntities_.begin() + (long)i);
        } else {
            ++i;
        }
    }

    // Синхронизируем футпринт-коллайдеры зданий под текущий список сущностей (для предсказания героя).
    syncBuildingColliders();
}

// Зеркало серверного GameWorld::blocksPath: какие здания физически блокируют движение (футпринт-
// бокс). Должно СОВПАДАТЬ с сервером — иначе предсказание героя разойдётся с авторитетом. Спавнер
// НЕ блокирует (из него выходят враги), враг — тоже.
static bool blocksHeroPath(EntityType t) {
    return t == EntityType::Generator || t == EntityType::Storage ||
           t == EntityType::Tower || t == EntityType::Core;
}

void Scene::syncBuildingColliders() {
    if (!collision_) return;
    // Здания статичны: бокс ставим один раз по позиции появления, геометрия ТА ЖЕ, что у сервера
    // (GameWorld::attachFootprint) — центр {x, 0.5, z}, полуразмеры {cell/2, 0.5, cell/2}.
    const float h = grid_.cell * 0.5f;
    for (const RemoteEntity& r : remoteEntities_) {
        if (!blocksHeroPath((EntityType)r.type)) continue;
        if (buildingColliders_.count(r.id) != 0) continue;
        uint32_t box = collision_->addBox(Vec3{r.ch.position.x, 0.5f, r.ch.position.z},
                                          Vec3{h, 0.5f, h});
        if (box != 0) buildingColliders_[r.id] = box;
    }
    // Убрать боксы зданий, которых больше нет (разрушены / матч-рестарт / выход из сессии).
    for (auto it = buildingColliders_.begin(); it != buildingColliders_.end();) {
        bool alive = false;
        for (const RemoteEntity& r : remoteEntities_)
            if (r.id == it->first && blocksHeroPath((EntityType)r.type)) { alive = true; break; }
        if (alive) {
            ++it;
        } else {
            collision_->removeBox(it->second);
            it = buildingColliders_.erase(it);
        }
    }
}

void Scene::hostGame() {
    leaveGame();
    if (server_.start(kNetPort)) {
        server_.configureWorld(sceneDesc_);  // тот же мир коллизий, что у клиента
        client_.connect("127.0.0.1", kNetPort);
        host_ = true;
        inputSeq_ = 0;
    }
}

void Scene::joinGame(const char* ip) {
    leaveGame();
    std::strncpy(serverIp_, ip, sizeof(serverIp_) - 1);
    serverIp_[sizeof(serverIp_) - 1] = '\0';
    client_.connect(serverIp_, kNetPort);
    host_ = false;
    wantReconnect_ = true;  // при обрыве пытаемся вернуться на тот же сервер
    reconnectTimer_ = 0.0f;
    inputSeq_ = 0;
}

void Scene::leaveGame() {
    client_.disconnect();
    server_.stop();
    host_ = false;
    wantReconnect_ = false;  // сознательный выход — не переподключаемся
    reconnectTimer_ = 0.0f;
    remoteEntities_.clear();
    syncBuildingColliders();  // снять все футпринт-боксы зданий (remoteEntities_ уже пуст)
    dyingMobs_.clear();
    pending_.clear();
    localTeam_ = 0;
    localHp_ = 1.0f;      // вне сессии герой «жив» (иначе своя лиса не рисовалась бы)
    localRespawn_ = 0.0f;
}

void Scene::onPointer(float x, float y, bool pressed) {
    joystick_.onPointer(x, y, pressed);
}

// --- Мультитач twin-stick: палец в левой половине владеет левым стиком (герой), в правой —
// правым (камера). Стик держит палец по id, пока тот не отпущен; оба работают одновременно.
void Scene::onTouchDown(int id, float x, float y, float vw, float vh) {
    bool left = x < vw * 0.5f;
    if (left) {
        if (movePointer_ < 0) { movePointer_ = id; joystick_.onPointer(x, y, true); }
    } else {
        if (camPointer_ < 0) { camPointer_ = id; camJoystick_.onPointer(x, y, true); }
    }
}
void Scene::onTouchMove(int id, float x, float y) {
    if (id == movePointer_) joystick_.onPointer(x, y, true);
    else if (id == camPointer_) camJoystick_.onPointer(x, y, true);
}
void Scene::onTouchUp(int id) {
    if (id == movePointer_) { joystick_.onPointer(0.0f, 0.0f, false); movePointer_ = -1; }
    else if (id == camPointer_) { camJoystick_.onPointer(0.0f, 0.0f, false); camPointer_ = -1; }
}

RenderFrame Scene::render(float alpha, float aspect, float renderDt) {
    // Камера ¾-вида следует за ИНТЕРПОЛИРОВАННОЙ позицией цели; азимут/зум двигает игрок
    // (правый стик на Android либо стрелки на десктопе). Наклон камеры фиксирован.
    Vec3 focusPos = player_.prevPosition + (player_.position - player_.prevPosition) * alpha;

    constexpr float kCamYawSpeed = 2.4f;    // рад/с при полном отклонении
    constexpr float kCamZoomSpeed = 14.0f;  // world/с при полном отклонении
    float camYaw = camJoystick_.active ? camJoystick_.x : extCamYaw_;
    float camZoom = camJoystick_.active ? camJoystick_.y : extCamZoom_;
    camera_.rotate(camYaw * kCamYawSpeed * renderDt);   // вправо -> камера поворачивается вправо
    camera_.zoom(-camZoom * kCamZoomSpeed * renderDt);  // вверх -> приблизить (меньше distance)
    camera_.follow(focusPos, renderDt);

    RenderFrame frame;
    frame.view = camera_.view();
    frame.proj = camera_.proj(aspect);
    frame.cameraPos = camera_.eye();
    frame.lightDir = normalize(lightDir_);  // из файла сцены; правится в GUI
    frame.shadowsEnabled = shadowsEnabled_;
    frame.shadowBias = shadowBias_;
    frame.shadowRadius = shadowRadius_;
    frame.fogColor = fogColor_;
    frame.fogDensity = fogDensity_;

    for (const GameObject& obj : objects_) {
        Transform t = obj.transform;
        t.rotation.y = obj.prevRotY + (obj.transform.rotation.y - obj.prevRotY) * alpha;
        frame.items.push_back({obj.mesh, obj.material, t.matrix()});
    }
    if (!chars_.empty() && !heroDead()) {  // повержённого героя не рисуем (появится на респауне)
        // Свой аватар: интерполяция prev -> current по alpha.
        Vec3 p = player_.prevPosition + (player_.position - player_.prevPosition) * alpha;
        float yaw = lerpAngle(player_.prevFacingYaw, player_.facingYaw, alpha);
        float ap = player_.prevAnimParam + (player_.animParam - player_.prevAnimParam) * alpha;
        float at = player_.prevAnimTime + (player_.animTime - player_.prevAnimTime) * alpha;
        float atk = player_.prevAttackTime + (player_.attackTime - player_.prevAttackTime) * alpha;
        frame.skinned.push_back(makeSkinnedItem(chars_, localCharIndex_, p, yaw, ap, at, atk));
    }

    // Удалённые игроки: рендерим «прошлое» на kInterpDelay назад, интерполируя
    // между двумя снапшотами из буфера. Это и есть snapshot interpolation.
    const double kInterpDelay = 0.1;  // сек буфера — гасит джиттер/потери
    double renderTime = simClock_ + (double)(alpha * tickDt_) - kInterpDelay;
    for (RemoteEntity& r : remoteEntities_) {
        const std::vector<TimedState>& buf = r.buffer;
        if (!buf.empty()) {
            TimedState st = buf.back();  // по умолчанию — свежайший
            if (renderTime <= buf.front().t) {
                st = buf.front();
            } else if (renderTime < buf.back().t) {
                for (size_t i = 0; i + 1 < buf.size(); ++i) {
                    if (renderTime >= buf[i].t && renderTime < buf[i + 1].t) {
                        const TimedState& a = buf[i];
                        const TimedState& b = buf[i + 1];
                        float f = (float)((renderTime - a.t) / (b.t - a.t));
                        st.pos = a.pos + (b.pos - a.pos) * f;
                        st.yaw = lerpAngle(a.yaw, b.yaw, f);
                        st.anim = a.anim + (b.anim - a.anim) * f;
                        st.attack = a.attack + (b.attack - a.attack) * f;
                        break;
                    }
                }
            }
            r.ch.position = st.pos;
            r.ch.facingYaw = st.yaw;
            r.ch.animParam = st.anim;
            r.ch.attackTime = st.attack;  // остаток каста (интерполированный) для рендера
        }
        r.ch.animTime += renderDt;  // фаза анимации крутится локально

        // Рендер по типу: Hero/Enemy — скиннинг-модель из реестра, остальные — generic-меш.
        EntityType et = (EntityType)r.type;
        if (et == EntityType::Hero) {
            // Чужой герой: союзник (та же команда) — синеватый, противник (PvP) — красный.
            // Свой герой рисуется отдельно (player_) обычным цветом — так их не спутать.
            SkinnedItem it =
                makeSkinnedItem(chars_, r.charType, r.ch.position, r.ch.facingYaw, r.ch.animParam,
                                r.ch.animTime, r.ch.attackTime);
            it.color = (r.team == localTeam_) ? Vec3{0.55f, 0.75f, 1.0f}   // союзник
                                              : Vec3{1.0f, 0.45f, 0.45f};  // враг
            frame.skinned.push_back(it);
        } else if (et == EntityType::Enemy && !mobs_.empty()) {
            // Моб-скелет: тип по charType (сервер выставляет). attackTime>0 (флаг с сервера) —
            // бьёт ядро/героя: лупим attack-клип; иначе идёт -> walk. Смерть — локальный
            // «труп» после исчезновения из снапшота (см. ниже + applySnapshot).
            int mi = (int)((uint32_t)r.charType % (uint32_t)mobs_.size());
            SkinnedItem it;
            if (r.ch.attackTime > 0.0f && mobs_[mi].attackClip >= 0) {
                // oneShotClip с растущим animTime -> fmod-цикл: удар повторяется, пока в упоре.
                it = makeSkinnedItem(mobs_, mi, r.ch.position, r.ch.facingYaw, 0.0f, 0.0f, 0.0f,
                                     mobs_[mi].attackClip, r.ch.animTime);
            } else {
                it = makeSkinnedItem(mobs_, mi, r.ch.position, r.ch.facingYaw, 1.0f /*walk*/,
                                     r.ch.animTime, 0.0f);
            }
            frame.skinned.push_back(it);
        } else if (et == EntityType::Projectile && projMesh_ != 0) {
            // Снаряд башни (серверная сущность): тонкий вытянутый болт вдоль полёта (yaw с сервера).
            Mat4 m = Mat4::translation(r.ch.position) * Mat4::rotationY(r.ch.facingYaw) *
                     Mat4::scale({0.08f, 0.08f, 0.6f});
            frame.items.push_back({projMesh_, projMat_, m});
        } else {
            const EntityVisual& v = visual(et);
            if (v.mesh != 0)
                frame.items.push_back({v.mesh, v.material,
                    Mat4::translation(r.ch.position + Vec3{0.0f, v.yOffset, 0.0f})});
        }
    }

    // «Трупы» убитых мобов: клип смерти на месте гибели, затем убираем. Чисто клиентская
    // косметика (сервер сущность уже удалил) — заводится в applySnapshot.
    for (size_t i = 0; i < dyingMobs_.size();) {
        DyingMob& d = dyingMobs_[i];
        d.t += renderDt;
        if (d.t >= d.dur) { dyingMobs_.erase(dyingMobs_.begin() + (long)i); continue; }
        float ct = (d.t < d.dur * 0.999f) ? d.t : d.dur * 0.999f;  // не зацикливать
        frame.skinned.push_back(makeSkinnedItem(mobs_, d.charType, d.pos, d.yaw, 0.0f, 0.0f,
                                                0.0f, mobs_[d.charType].deathClip, ct));
        ++i;
    }

    // (Снаряды башен — серверные сущности EntityType::Projectile; рисуются в remote-цикле выше.)

    // Снап-подсветка сетки + призрак размещения (режим стройки).
    if (buildActive_) {
        int tcx, tcz;
        Vec3 tcenter;
        bool valid = computeGhost(tcx, tcz, tcenter);  // клетка перед героем + её валидность
        // Подсветку рисуем не по всей арене, а окном ±kBuildGridRadius клеток вокруг героя:
        // цель размещения всегда рядом (перед героем), а на больших полях перебор всей арены —
        // это тысячи тайлов и переполнение инстанс-буфера. Окно пересекаем с границами арены.
        constexpr int kBuildGridRadius = 4;  // 9x9 клеток вокруг героя (тюнится; при клик-плейсменте — вокруг курсора)
        int lo, hi;
        grid_.cellRange(lo, hi);  // границы арены (центр клетки внутри) — закрытая формула
        int hx = grid_.cellOf(player_.position.x), hz = grid_.cellOf(player_.position.z);
        int x0 = (hx - kBuildGridRadius < lo) ? lo : hx - kBuildGridRadius;
        int x1 = (hx + kBuildGridRadius > hi) ? hi : hx + kBuildGridRadius;
        int z0 = (hz - kBuildGridRadius < lo) ? lo : hz - kBuildGridRadius;
        int z1 = (hz + kBuildGridRadius > hi) ? hi : hz + kBuildGridRadius;
        // Тайлы (чуть над полом): занятая — красный, свободная — нейтральный; клетка под
        // призраком — цвет валидности, чтобы снап читался и на полу.
        for (int cz = z0; cz <= z1; ++cz)
            for (int cx = x0; cx <= x1; ++cx) {
                MaterialHandle mat = (cx == tcx && cz == tcz)
                                         ? (valid ? ghostOkMat_ : ghostBadMat_)
                                         : (cellOccupied(cx, cz) ? gridBusyMat_ : gridFreeMat_);
                Vec3 c = grid_.cellCenter(cx, cz);
                frame.items.push_back({gridTileMesh_, mat, Mat4::translation({c.x, 0.02f, c.z})});
            }
        // Призрак типа на целевой клетке (куб/сфера), зелёный/красный.
        const EntityVisual& v = visual(buildType_);
        frame.items.push_back({v.mesh, valid ? ghostOkMat_ : ghostBadMat_,
                               Mat4::translation(tcenter + Vec3{0.0f, v.yOffset, 0.0f})});
    }

    // HUD ставок героя (bitmap-шрифт — только ASCII; кириллический баннер — в ImGui-слое
    // через геттеры heroDead()/heroRespawnLeft()). Верхний левый угол, под FPS.
    if (client_.connected()) {
        char buf[48];
        if (heroDead()) {
            std::snprintf(buf, sizeof(buf), "DOWN - respawn %.0f", (double)std::ceil(localRespawn_));
            frame.hud.push_back({buf, 24.0f, 110.0f, 44.0f, {0.95f, 0.35f, 0.30f}});
        } else {
            std::snprintf(buf, sizeof(buf), "HP %.0f/%.0f", (double)localHp_, (double)localMaxHp_);
            frame.hud.push_back({buf, 24.0f, 74.0f, 28.0f, {0.70f, 0.95f, 0.70f}});
        }
    }
    return frame;
}

int Scene::remoteCount() const {
    int n = 0;
    for (const RemoteEntity& r : remoteEntities_)
        if ((EntityType)r.type == EntityType::Hero) ++n;  // только другие герои
    return n;
}

float Scene::resourceCurrent() const {
    float sum = 0.0f;  // только хранилища СВОЕЙ команды (aux из снапшотов)
    for (const RemoteEntity& r : remoteEntities_)
        if ((EntityType)r.type == EntityType::Storage && r.team == localTeam_) sum += r.aux;
    return sum;
}

float Scene::resourceCap() const {
    // Ёмкость хранилищ своей команды из ЖИВЫХ сущностей (снапшоты) — как и resourceCurrent,
    // иначе построенные в рантайме хранилища не учитывались бы (в sceneDesc_ их нет). Cap
    // одинаков для типа (из конфига), поэтому = число хранилищ × ёмкость из конфига.
    const float perStorage = config_.get(EntityType::Storage).cap;
    int count = 0;
    for (const RemoteEntity& r : remoteEntities_)
        if ((EntityType)r.type == EntityType::Storage && r.team == localTeam_) ++count;
    return (float)count * perStorage;
}

int Scene::matchPhase() const { return (int)client_.gamePhase(); }

float Scene::coreHp() const {
    for (const RemoteEntity& r : remoteEntities_)
        if ((EntityType)r.type == EntityType::Core) return r.hp;
    return -1.0f;  // ядра нет в снапшотах (не подключены / соло без сети)
}

float Scene::coreMaxHp() const { return config_.get(EntityType::Core).hp; }

const Scene::EntityVisual& Scene::visual(EntityType t) const {
    static const EntityVisual kNone;  // неизвестный/вне-диапазона тип -> пусто (не рисуется/не пикается)
    int i = (int)t;
    return (i >= 0 && i < kEntityVisualCount) ? visuals_[i] : kNone;
}

void Scene::onClick(float x, float y, float vw, float vh) {
    if (vw <= 0.0f || vh <= 0.0f) return;
    // Экран -> NDC -> мировой луч (unproject через inverse(proj*view)).
    float ndcX = 2.0f * x / vw - 1.0f;
    float ndcY = 1.0f - 2.0f * y / vh;  // экран вниз -> NDC вверх
    Mat4 invVP = inverse(camera_.proj(vw / vh) * camera_.view());
    auto unproj = [&](float nz) -> Vec3 {
        const float* m = invVP.m;  // column-major: out[row] = sum_col m[col*4+row]*v[col]
        float ox = m[0] * ndcX + m[4] * ndcY + m[8] * nz + m[12];
        float oy = m[1] * ndcX + m[5] * ndcY + m[9] * nz + m[13];
        float oz = m[2] * ndcX + m[6] * ndcY + m[10] * nz + m[14];
        float ow = m[3] * ndcX + m[7] * ndcY + m[11] * nz + m[15];
        if (ow != 0.0f) { ox /= ow; oy /= ow; oz /= ow; }
        return Vec3{ox, oy, oz};
    };
    Vec3 nearP = unproj(-1.0f);  // GL clip near (z=-1)
    Vec3 farP = unproj(1.0f);
    Vec3 origin = nearP;
    Vec3 dir = normalize(farP - nearP);

    // Луч vs сфера каждого выбираемого здания; берём ближайшее попадание.
    uint32_t best = 0;
    float bestT = 1e30f;
    for (const RemoteEntity& r : remoteEntities_) {
        const EntityVisual& v = visual((EntityType)r.type);
        if (!v.pickable) continue;
        Vec3 c = r.ch.position + Vec3{0.0f, v.yOffset, 0.0f};
        Vec3 oc = origin - c;
        float b = dot(oc, dir);
        float cc = dot(oc, oc) - v.pickRadius * v.pickRadius;
        float disc = b * b - cc;
        if (disc < 0.0f) continue;
        float sq = std::sqrt(disc);
        float tt = -b - sq;
        if (tt < 0.0f) tt = -b + sq;  // луч стартует внутри сферы
        if (tt < 0.0f) continue;
        if (tt < bestT) { bestT = tt; best = r.id; }
    }
    selectedId_ = best;  // 0 = мимо -> снять выделение
}

int Scene::selectedEntityType() const {
    for (const RemoteEntity& r : remoteEntities_)
        if (r.id == selectedId_) return (int)r.type;
    return -1;  // нет выделения или сущность исчезла
}

const BuildingInfo* Scene::selectedInfo() const {
    int t = selectedEntityType();
    if (t < 0) return nullptr;
    const BuildingInfo& bi = config_.get((EntityType)t);
    return bi.defined ? &bi : nullptr;
}

float Scene::selectedAux() const {
    for (const RemoteEntity& r : remoteEntities_)
        if (r.id == selectedId_) return r.aux;
    return 0.0f;
}

// --- Стройка (G3-B) ---
bool Scene::computeGhost(int& cx, int& cz, Vec3& center) const {
    // Клетка перед героем (по facing, ~1.5 клетки вперёд), снап на сетку.
    float yaw = player_.facingYaw;
    Vec3 fwd{std::sin(yaw), 0.0f, std::cos(yaw)};
    Vec3 p = player_.position + fwd * (grid_.cell * 1.5f);
    cx = grid_.cellOf(p.x);
    cz = grid_.cellOf(p.z);
    center = grid_.cellCenter(cx, cz);

    if (!grid_.inArena(cx, cz)) return false;             // вне зоны строительства
    if (cellOccupied(cx, cz)) return false;               // клетка занята зданием
    if (resourceCurrent() < config_.get(buildType_).cost) return false;  // не хватает ресурса
    return true;
}

bool Scene::cellOccupied(int cx, int cz) const {
    for (const RemoteEntity& r : remoteEntities_) {
        if (!visual((EntityType)r.type).building) continue;  // враг не занимает клетку
        if (grid_.cellOf(r.ch.position.x) == cx && grid_.cellOf(r.ch.position.z) == cz)
            return true;
    }
    return false;
}

bool Scene::buildGhostValid() const {
    int cx, cz;
    Vec3 c;
    return computeGhost(cx, cz, c);
}

const BuildingInfo* Scene::buildInfo(int type) const {
    if (type < 0 || type >= 8) return nullptr;
    return &config_.get((EntityType)type);
}

void Scene::beginBuild(int type) {
    buildType_ = (EntityType)type;
    buildActive_ = true;
    clearSelection();
}

void Scene::confirmBuild() {
    if (!buildActive_ || !client_.connected()) return;
    int cx, cz;
    Vec3 center;
    if (!computeGhost(cx, cz, center)) return;  // невалидно — не шлём запрос
    client_.sendBuild((uint8_t)buildType_, cx, cz);
    // Остаёмся в режиме — можно ставить дальше (сервер авторитетно применит/отвергнет).
}
