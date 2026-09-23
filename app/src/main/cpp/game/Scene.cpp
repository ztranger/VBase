#include "game/Scene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>

#include "engine/assets/AssetSource.h"
#include "engine/assets/Assets.h"
#include "engine/physics/CollisionWorld.h"
#include "game/CharacterRoster.h"
#include "game/TowerRoster.h"
#include "game/Grid.h"
#include "engine/core/Log.h"
#include "engine/core/Renderer.h"
#include "game/SceneLoader.h"
#include "game/BuildRules.h"  // blocksPath/footprintBox — общие с сервером (GameWorld)
#include "game/CombatFx.h"    // k*/isCombatType/combatHeadHeight — боевая косметика (общие с ClientWorld)

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
    currentScenePath_ = scenePath ? scenePath : "";
    loadSceneManifest(assets, "config/scenes.cfg");  // список сцен для меню (не критично, если нет)

    SceneDesc desc;
    if (!loadSceneDesc(assets, scenePath, desc)) {
        LOGE("Не удалось загрузить сцену: %s", scenePath);
        return;  // пустая сцена — ошибки уже в логе
    }
    sceneDesc_ = desc;  // сохраняем: host отдаст ту же геометрию серверу; rebuildGraphics переиспользует
    // Конфиг зданий (параметры + тексты панели). Параметры применяем к зданиям сцены —
    // единый источник настроек: сцена размещает, конфиг задаёт rate/cap/…
    loadBuildingConfig(assets, "config/buildings.cfg", config_);
    applyBuildingConfig(sceneDesc_, config_);

    // --- Ростер персонажей: ДАННЫЕ (в sceneDesc_). GPU-загрузка — ниже в createGpuResources
    // (из sceneDesc_.heroTypes/enemyTypes), чтобы rebuildGraphics поднимал модели без чтения
    // конфигов. Определяем ДО валидации, чтобы санитизация покрыла и статы ростера. ---
    int defChar = 0;
    if (sceneDesc_.player.present) {
        std::vector<CharacterDesc> roster;
        if (!loadCharacterRoster(assets, "config/characters.cfg", roster)) {
            CharacterDesc c;
            c.id = c.name = "player";
            c.model = sceneDesc_.player.model;
            c.scale = sceneDesc_.player.scale;
            c.yawOffset = sceneDesc_.player.yawOffset;
            c.hide = sceneDesc_.player.hideNodes;
            roster.push_back(std::move(c));
        }
        sceneDesc_.heroTypes = roster;  // статы героев -> локальному серверу И источник GPU-загрузки
        for (size_t i = 0; i < roster.size(); ++i)
            if (roster[i].model == sceneDesc_.player.model) { defChar = (int)i; break; }

        std::vector<CharacterDesc> mobRoster;
        if (loadCharacterRoster(assets, "config/enemies.cfg", mobRoster))
            sceneDesc_.enemyTypes = mobRoster;
    }

    // Виды башен/ловушек (towers.cfg): нужны и клиенту (цена/палитра/тинт), и локальному
    // серверу в host-режиме (host отдаёт sceneDesc_ серверу). Индекс = сетевой вид (kind).
    loadTowerRoster(assets, "config/towers.cfg", sceneDesc_.towerTypes);

    // P2-12: санитизируем недоверенное описание ДО использования (сетка-делитель, коллайдеры/
    // капсула в Jolt, статы). Дальше читаем ТОЛЬКО из sceneDesc_ (уже безопасного).
    validateSceneDesc(sceneDesc_);

    grid_ = sceneDesc_.grid;  // строительная сетка (после клампа cell>0)
    localMaxHp_ = config_.get(EntityType::Hero).hp;  // для HUD-бара героя
    if (!(localMaxHp_ > 0.0f)) localMaxHp_ = 100.0f;  // ловит и NaN

    // Свет и камера — из санитизированного описания.
    lightDir_ = sceneDesc_.lightDir;
    camera_.distance = sceneDesc_.camera.distance;
    camera_.pitch = sceneDesc_.camera.pitch;
    camera_.lookHeight = sceneDesc_.camera.lookHeight;
    camera_.fovY = sceneDesc_.camera.fovY;
    camera_.nearZ = sceneDesc_.camera.nearZ;
    camera_.farZ = sceneDesc_.camera.farZ;

    // --- Физика + контроллер героя: мир коллизий (боксы сцены) + капсула игрока — в ClientWorld
    // (P2-18 шаг 4). Санитизированное описание уже в sceneDesc_.
    clientWorld_.configure(sceneDesc_, player_);

    // --- GPU-ресурсы (меши/текстуры/материалы/объекты/визуалы/ростер) ---
    createGpuResources(renderer, assets);

    if (sceneDesc_.player.present)
        selectCharacter(defChar);  // индекс + player_.maxSpeed из chars_[def].speed (нужны загруженные chars_)
}

// GPU-часть сборки: создаёт ресурсы рендера из СОХРАНЁННОГО описания (sceneDesc_/config_/grid_).
// НЕ трогает физику, сеть, сессию и позицию героя — поэтому её можно вызвать повторно
// (rebuildGraphics) после пересоздания рендера, сохранив идущий матч. Все держатели хэндлов
// (objects_/visuals_/chars_/mobs_/ghost/proj/grid) переприсваиваются здесь вместе.
void Scene::createGpuResources(Renderer& renderer, AssetSource& assets) {
    objects_.clear();
    chars_.clear();
    mobs_.clear();
    const SceneDesc& desc = sceneDesc_;  // читаем ретейнутое описание (общее для build и rebuildGraphics)

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

    // Локальный AABB меша (для пикинга/подсветки в редакторе).
    auto aabbOf = [](const MeshData& md, Vec3& mn, Vec3& mx) {
        if (md.vertices.empty()) { mn = mx = Vec3{0.0f, 0.0f, 0.0f}; return; }
        mn = mx = Vec3{md.vertices[0].px, md.vertices[0].py, md.vertices[0].pz};
        for (const Vertex& v : md.vertices) {
            mn.x = std::min(mn.x, v.px); mn.y = std::min(mn.y, v.py); mn.z = std::min(mn.z, v.pz);
            mx.x = std::max(mx.x, v.px); mx.y = std::max(mx.y, v.py); mx.z = std::max(mx.z, v.pz);
        }
    };

    // --- Меши (имя -> handle) ---
    std::unordered_map<std::string, MeshHandle> meshMap;
    std::unordered_map<std::string, std::pair<Vec3, Vec3>> meshBounds;  // локальный AABB по имени
    for (const MeshSpec& m : desc.meshes) {
        MeshData md;
        switch (m.kind) {
            case MeshSpec::Plane:  md = makePlane(m.a, m.b); break;
            case MeshSpec::Cube:   md = makeCube(m.a); break;
            case MeshSpec::Sphere: md = makeSphere(m.a, m.stacks, m.slices); break;
        }
        meshMap[m.name] = renderer.createMesh(md);
        Vec3 mn, mx;
        aabbOf(md, mn, mx);
        meshBounds[m.name] = {mn, mx};
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
    // Кэш статичных glTF-пропсов (mesh+material) по ключу path|tex|shader: уровень ставит много
    // копий одного ассета — грузим/заливаем в GPU один раз.
    // Кэшируем geometry + альбедо по glb-пути|tex (материал создаём отдельно — он зависит ещё от
    // shader/color, которые правит редактор поштучно).
    struct GlbEntry { MeshHandle mesh = 0; TextureHandle albedo = 0; Vec3 mn, mx; bool ok = false; };
    std::unordered_map<std::string, GlbEntry> glbCache;
    auto loadGlb = [&](const ObjectSpec& os) -> GlbEntry {
        const std::string key = os.model + "|" + os.modelTex;
        auto it = glbCache.find(key);
        if (it != glbCache.end()) return it->second;
        GlbEntry res;
        MeshData md;
        TextureData embedded;
        bool hasTex = false;
        if (loadGltfStatic(assets, os.model.c_str(), md, embedded, hasTex)) {
            res.mesh = renderer.createMesh(md);
            aabbOf(md, res.mn, res.mx);
            if (!os.modelTex.empty()) {  // внешняя текстура-атлас переопределяет встроенную
                TextureData ext;
                if (loadImageAsset(assets, os.modelTex.c_str(), ext)) res.albedo = renderer.createTexture(ext);
            } else if (hasTex) {
                res.albedo = renderer.createTexture(embedded);
            }
            res.ok = true;
        } else {
            LOGW("Объект: glTF-модель %s не загрузилась — пропущен", os.model.c_str());
        }
        glbCache[key] = res;
        return res;
    };

    const float pi = 3.14159265358979323846f;
    for (size_t si = 0; si < desc.objects.size(); ++si) {
        const ObjectSpec& os = desc.objects[si];
        MeshHandle mh = 0;
        MaterialHandle mah = 0;
        TextureHandle alb = 0;
        Vec3 aabbMn{0.0f, 0.0f, 0.0f}, aabbMx{0.0f, 0.0f, 0.0f};
        if (!os.model.empty()) {  // glTF-декор: своя geometry + материал (shader/color объекта)
            GlbEntry e = loadGlb(os);
            if (!e.ok) continue;  // не загрузилась — не плодим пустышки
            mh = e.mesh;
            alb = e.albedo;
            aabbMn = e.mn;
            aabbMx = e.mx;
            MaterialDesc mat;
            mat.shader = os.shader;
            mat.baseColor = os.color;  // тинт (по умолчанию белый = цвет из текстуры)
            mat.albedo = alb;
            mah = renderer.createMaterial(mat);
        } else {
            mh = meshH(os.mesh);
            mah = matH(os.material);
            auto bit = meshBounds.find(os.mesh);
            if (bit != meshBounds.end()) { aabbMn = bit->second.first; aabbMx = bit->second.second; }
        }
        if (os.ring) {
            for (int k = 0; k < os.ringCount; ++k) {
                float a = 2.0f * pi * (float)k / (float)(os.ringCount > 0 ? os.ringCount : 1);
                GameObject c;
                c.mesh = mh;
                c.material = mah;
                c.transform.position = {std::cos(a) * os.ringRadius, os.ringY, std::sin(a) * os.ringRadius};
                c.transform.rotation = os.rot;
                c.transform.scale = os.scale;
                c.spin = os.spin;
                c.prevRotY = os.rot.y;
                c.aabbMin = aabbMn;
                c.aabbMax = aabbMx;
                c.albedo = alb;
                c.specIndex = -1;  // копии кольца не редактируются поштучно (MVP)
                objects_.push_back(c);
            }
        } else {
            GameObject o;
            o.mesh = mh;
            o.material = mah;
            o.transform.position = os.pos;
            o.transform.rotation = os.rot;
            o.transform.scale = os.scale;
            o.spin = os.spin;
            o.prevRotY = os.rot.y;
            o.aabbMin = aabbMn;
            o.aabbMax = aabbMx;
            o.albedo = alb;
            o.specIndex = (int)si;
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
                if ((EntityType)t == EntityType::Tower) towerTex_ = tex;  // атлас для тинта видов башен
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
    // Маркер точки спавна (только редактор): маленький куб + яркий Unlit-материал (спавны невидимы).
    // Цвет — по команде, чтобы стороны PvP различались в 3D-виде: 0 нейтр/кооп, 1/2/3 — стороны.
    editorMarkerMesh_ = renderer.createMesh(makeCube(0.8f));
    {
        const Vec3 teamCol[kEditorTeamColors] = {
            {0.35f, 0.85f, 1.0f},   // 0 — циан (нейтр/кооп)
            {0.35f, 0.55f, 0.95f},  // 1 — синий
            {0.95f, 0.40f, 0.40f},  // 2 — красный
            {0.45f, 0.90f, 0.50f},  // 3 — зелёный
        };
        for (int t = 0; t < kEditorTeamColors; ++t) {
            MaterialDesc md;
            md.shader = ShaderType::Unlit;
            md.baseColor = teamCol[t];
            editorMarkerMat_[t] = renderer.createMaterial(md);
        }
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

    // --- Скиннинг-ростер -> GPU (из сохранённых статов; индекс = сетевой charType). Слот для
    // КАЖДОЙ модели ростера заводится всегда (loadRosterModels), поэтому размеры chars_/mobs_
    // и индекс localCharIndex_ стабильны между build() и rebuildGraphics(). ---
    if (!sceneDesc_.heroTypes.empty())
        loadRosterModels(renderer, assets, sceneDesc_.heroTypes, chars_);
    if (!sceneDesc_.enemyTypes.empty())
        loadRosterModels(renderer, assets, sceneDesc_.enemyTypes, mobs_);

    // --- Материалы видов защиты (towers.cfg): тинт башни (модель-пилон + атлас) и болта; плита
    // ловушки. Индекс = kind. Башни красим тинтом поверх атласа (towerTex_), ловушки/болты —
    // сплошным цветом. Создаём даже без ростера пустыми (рендер тогда откатится на базовый). ---
    trapMesh_ = renderer.createMesh(makeCube(1.0f));  // масштабируется в низкую плиту в рендере
    towerKindMat_.clear();
    projKindMat_.clear();
    for (const TowerDesc& d : sceneDesc_.towerTypes) {
        MaterialDesc tm;  // корпус башни/ловушки
        if (d.entity == EntityType::Tower && towerTex_ != 0) {
            tm.shader = ShaderType::Phong;
            tm.albedo = towerTex_;      // атлас пилона, крашеный тинтом вида
            tm.baseColor = d.color;
        } else {
            tm.shader = ShaderType::Lit;  // ловушка (или башня без атласа) — сплошной цвет
            tm.baseColor = d.color;
        }
        towerKindMat_.push_back(renderer.createMaterial(tm));

        MaterialDesc pm;  // болт
        pm.shader = ShaderType::Unlit;
        pm.baseColor = d.color;
        projKindMat_.push_back(renderer.createMaterial(pm));
    }
}

void Scene::editorReloadFromDesc(const SceneDesc& desc, Renderer& renderer, AssetSource& assets) {
    sceneDesc_ = desc;              // новое описание (undo/redo редактора)
    lightDir_ = desc.lightDir;      // свет
    createGpuResources(renderer, assets);  // пересоздаёт objects_/визуалы/рендер зданий из sceneDesc_
}

void Scene::rebuildGraphics(Renderer& renderer, AssetSource& assets) {
    // Только GPU-ресурсы под новый рендер; игровая сессия (сеть/матч/предсказание/физика)
    // и выбранный персонаж (localCharIndex_/player_) сохраняются. Требует прошедшего build().
    createGpuResources(renderer, assets);
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
        pm.damage = c.damage;  // для брифинга выбора героя (Lobby); клиент бой не считает
        pm.range = c.range;
        pm.ranged = c.ranged;
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
                                   float attackTime, int oneShotClip, float oneShotTime,
                                   float locoPhase) const {
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
        // Атака перекрывает локомоцию: клип каста растягивается ровно на окно kAttackDuration.
        // Авто-бленд: attack плавно въезжает/выезжает поверх ведущего клипа локомоции (~0.15с),
        // без резкого щелчка (событийная анимация).
        } else if (attackTime > 0.0f && pm.attackClip >= 0 && pm.attackClipDur > 0.0f) {
            float frac = 1.0f - attackTime / Character::kAttackDuration;  // 0..1 прогресс каста
            frac = frac < 0.0f ? 0.0f : (frac > 0.999f ? 0.999f : frac);  // без зацикливания
            const float fade = 0.15f / Character::kAttackDuration;        // доля прогресса на фейд
            float aw = 1.0f;                                             // вес attack (0=локомоция,1=атака)
            if (frac < fade) aw = frac / fade;
            else if (frac > 1.0f - fade) aw = (1.0f - frac) / fade;
            int loco = animParam < 0.5f ? pm.idleClip : (animParam < 1.5f ? pm.walkClip : pm.runClip);
            pm.model.sampleBlend(loco, animTime, pm.attackClip, frac * pm.attackClipDur, aw, item.joints);
        } else if (animParam <= 0.01f) {  // Клипы выбраны по имени при загрузке.
            pm.model.sampleAnimation(pm.idleClip, animTime, item.joints);
        } else if (animParam <= 1.0f) {
            pm.model.sampleBlend(pm.idleClip, animTime, pm.walkClip, animTime, animParam, item.joints);
        } else {
            // walk<->run: клипы РАЗНОЙ длительности. Берём оба в ОДНОЙ нормализованной фазе
            // цикла (locoPhase) -> стопы совпадают (иначе плывут). Каденс — по скорости (locoRate).
            float wd = pm.model.animations[pm.walkClip].duration;
            float rd = pm.model.animations[pm.runClip].duration;
            if (locoPhase >= 0.0f && wd > 0.0f && rd > 0.0f) {
                float ph = locoPhase - std::floor(locoPhase);  // [0,1)
                pm.model.sampleBlend(pm.walkClip, ph * wd, pm.runClip, ph * rd, animParam - 1.0f, item.joints);
            } else {  // нет фазы (моб/превью) — как раньше, от общего времени
                pm.model.sampleBlend(pm.walkClip, animTime, pm.runClip, animTime, animParam - 1.0f, item.joints);
            }
        }
    }
    return item;
}

float Scene::locoRate(const std::vector<PlayerModel>& reg, int index, float animParam) const {
    if (index < 0 || index >= (int)reg.size()) return 0.0f;
    const PlayerModel& pm = reg[index];
    if (pm.model.animations.empty()) return 0.0f;
    auto dur = [&](int clip) {
        return (clip >= 0 && clip < (int)pm.model.animations.size())
                   ? pm.model.animations[clip].duration : 0.0f;
    };
    float wd = dur(pm.walkClip), rd = dur(pm.runClip);
    if (wd <= 0.0f) return 0.0f;
    // Каденс: длительность цикла = walkDur при animParam<=1, далее бленд к runDur (быстрее).
    float t = animParam - 1.0f; t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    float cyc = (animParam <= 1.0f || rd <= 0.0f) ? wd : wd + (rd - wd) * t;
    return cyc > 0.0f ? 1.0f / cyc : 0.0f;
}

void Scene::selectCharacter(int i) {
    if (chars_.empty()) { localCharIndex_ = 0; return; }
    if (i < 0) i = 0;
    if (i >= (int)chars_.size()) i = (int)chars_.size() - 1;
    localCharIndex_ = i;
    session_.setCharType((uint8_t)i);  // сервер положит в снапшот -> чужие нарисуют нашей моделью
    // Статы выбранного героя: скорость — в предсказание (должна совпадать с сервером, иначе
    // реконсиляция дёргала бы), максимум hp — для HUD (текущий hp авторитетно из снапшота).
    if (chars_[i].speed > 0.0f) player_.maxSpeed = chars_[i].speed;
    if (chars_[i].hp > 0.0f) localMaxHp_ = chars_[i].hp;
}

// Экран входа в бой (Lobby): выбранный персонаж в origin, idle-анимация, медленное вращение.
// Мир НЕ рисуем — фон = чистый цвет очистки. Главный цикл зовёт это в рендер-пути CharacterPreview.
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
    return frame;
}

RenderFrame Scene::renderEditor(const Mat4& view, const Mat4& proj, const Vec3& eye) const {
    RenderFrame frame;
    frame.view = view;
    frame.proj = proj;
    frame.cameraPos = eye;
    frame.lightDir = normalize(lightDir_);
    frame.shadowsEnabled = shadowsEnabled_;
    frame.shadowBias = shadowBias_;
    frame.shadowRadius = shadowRadius_;
    frame.fogColor = {std::pow(sceneDesc_.horizonColor.x, 2.2f),  // цвет горизонта (тинт земли в редакторе)
                      std::pow(sceneDesc_.horizonColor.y, 2.2f),
                      std::pow(sceneDesc_.horizonColor.z, 2.2f)};
    frame.fogDensity = 0.0f;

    // Статичные объекты сцены (включая пол и glTF-декор) — как есть, без интерполяции спина.
    for (const GameObject& obj : objects_)
        frame.items.push_back({obj.mesh, obj.material, obj.transform.matrix()});

    // Здания базы: офлайн (без снапшотов) рисуем по описанию сцены их визуалом из таблицы типов.
    auto typeOf = [](BuildingSpec::Kind k) -> EntityType {
        switch (k) {
            case BuildingSpec::Generator: return EntityType::Generator;
            case BuildingSpec::Storage:   return EntityType::Storage;
            case BuildingSpec::Spawner:   return EntityType::Spawner;
            case BuildingSpec::Tower:     return EntityType::Tower;
            case BuildingSpec::Core:      return EntityType::Core;
        }
        return EntityType::Core;
    };
    for (const BuildingSpec& b : sceneDesc_.buildings) {
        const EntityVisual& v = visual(typeOf(b.kind));
        if (v.mesh != 0)
            frame.items.push_back(
                {v.mesh, v.material, Mat4::translation(b.pos + Vec3{0.0f, v.yOffset, 0.0f})});
    }
    // Маркеры точек спавна (невидимы в игре) — маленькие кубы, чтобы их можно было выбрать/двигать.
    // Цвет куба = команда точки спавна (различаем стороны PvP визуально).
    if (editorMarkerMesh_ != 0)
        for (const SpawnSpec& s : sceneDesc_.spawns) {
            const int ti = (s.team < kEditorTeamColors) ? (int)s.team : 0;
            frame.items.push_back({editorMarkerMesh_, editorMarkerMat_[ti],
                                   Mat4::translation(s.pos + Vec3{0.0f, 0.4f, 0.0f})});
        }
    return frame;
}

namespace {
// Локальный AABB меша (перебор вершин).
void aabbOfMesh(const MeshData& md, Vec3& mn, Vec3& mx) {
    if (md.vertices.empty()) { mn = mx = Vec3{0.0f, 0.0f, 0.0f}; return; }
    mn = mx = Vec3{md.vertices[0].px, md.vertices[0].py, md.vertices[0].pz};
    for (const Vertex& v : md.vertices) {
        mn.x = std::min(mn.x, v.px); mn.y = std::min(mn.y, v.py); mn.z = std::min(mn.z, v.pz);
        mx.x = std::max(mx.x, v.px); mx.y = std::max(mx.y, v.py); mx.z = std::max(mx.z, v.pz);
    }
}
// Мировой AABB объекта: 8 углов локального AABB через модельную матрицу -> охватывающий бокс.
void worldAabb(const Mat4& m, const Vec3& lmn, const Vec3& lmx, Vec3& wmn, Vec3& wmx) {
    bool first = true;
    for (int i = 0; i < 8; ++i) {
        Vec3 c{(i & 1) ? lmx.x : lmn.x, (i & 2) ? lmx.y : lmn.y, (i & 4) ? lmx.z : lmn.z};
        Vec3 w{m.m[0] * c.x + m.m[4] * c.y + m.m[8] * c.z + m.m[12],
               m.m[1] * c.x + m.m[5] * c.y + m.m[9] * c.z + m.m[13],
               m.m[2] * c.x + m.m[6] * c.y + m.m[10] * c.z + m.m[14]};
        if (first) { wmn = wmx = w; first = false; }
        else {
            wmn.x = std::min(wmn.x, w.x); wmn.y = std::min(wmn.y, w.y); wmn.z = std::min(wmn.z, w.z);
            wmx.x = std::max(wmx.x, w.x); wmx.y = std::max(wmx.y, w.y); wmx.z = std::max(wmx.z, w.z);
        }
    }
}
// Пересечение луча с AABB (slab). Возвращает t входа (>=0) или -1, если промах.
float rayAabb(const Vec3& ro, const Vec3& rd, const Vec3& mn, const Vec3& mx) {
    float tmin = -1e30f, tmax = 1e30f;
    const float o[3] = {ro.x, ro.y, ro.z}, d[3] = {rd.x, rd.y, rd.z};
    const float lo[3] = {mn.x, mn.y, mn.z}, hi[3] = {mx.x, mx.y, mx.z};
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(d[i]) < 1e-8f) {
            if (o[i] < lo[i] || o[i] > hi[i]) return -1.0f;  // параллельно и вне плиты
        } else {
            float t1 = (lo[i] - o[i]) / d[i], t2 = (hi[i] - o[i]) / d[i];
            if (t1 > t2) std::swap(t1, t2);
            tmin = std::max(tmin, t1);
            tmax = std::min(tmax, t2);
            if (tmin > tmax) return -1.0f;
        }
    }
    return (tmax < 0.0f) ? -1.0f : (tmin >= 0.0f ? tmin : tmax);
}
// Пересечение луча со сферой (center,r). Возвращает t входа (>=0) или -1.
float raySphere(const Vec3& ro, const Vec3& rd, const Vec3& c, float r) {
    Vec3 oc = ro - c;
    float b = dot(oc, rd);
    float cc = dot(oc, oc) - r * r;
    float disc = b * b - cc;
    if (disc < 0.0f) return -1.0f;
    float t = -b - std::sqrt(disc);
    if (t < 0.0f) t = -b + std::sqrt(disc);
    return t < 0.0f ? -1.0f : t;
}
// EntityType по типу здания сцены (для визуала/пикинга).
EntityType buildingType(BuildingSpec::Kind k) {
    switch (k) {
        case BuildingSpec::Generator: return EntityType::Generator;
        case BuildingSpec::Storage:   return EntityType::Storage;
        case BuildingSpec::Spawner:   return EntityType::Spawner;
        case BuildingSpec::Tower:     return EntityType::Tower;
        case BuildingSpec::Core:      return EntityType::Core;
    }
    return EntityType::Core;
}
BuildingSpec::Kind buildingKind(int type) {
    switch ((EntityType)type) {
        case EntityType::Generator: return BuildingSpec::Generator;
        case EntityType::Storage:   return BuildingSpec::Storage;
        case EntityType::Spawner:   return BuildingSpec::Spawner;
        case EntityType::Tower:     return BuildingSpec::Tower;
        default:                    return BuildingSpec::Core;
    }
}
}  // namespace

bool Scene::editorPickObject(const Vec3& ro, const Vec3& rd, int& idx, float& t) const {
    int best = -1;
    float bestT = 1e30f;
    for (const GameObject& o : objects_) {
        if (o.specIndex < 0) continue;  // редактируем только одиночные объекты
        Vec3 wmn, wmx;
        worldAabb(o.transform.matrix(), o.aabbMin, o.aabbMax, wmn, wmx);
        float th = rayAabb(ro, rd, wmn, wmx);
        if (th >= 0.0f && th < bestT) { bestT = th; best = o.specIndex; }
    }
    idx = best;
    t = bestT;
    return best >= 0;
}

bool Scene::editorObjectMatrix(int specIndex, Mat4& out) const {
    for (const GameObject& o : objects_)
        if (o.specIndex == specIndex) { out = o.transform.matrix(); return true; }
    return false;
}

bool Scene::editorGetTransform(int specIndex, Vec3& pos, Vec3& rot, Vec3& scale) const {
    for (const GameObject& o : objects_)
        if (o.specIndex == specIndex) {
            pos = o.transform.position;
            rot = o.transform.rotation;
            scale = o.transform.scale;
            return true;
        }
    return false;
}

void Scene::editorSetTransform(int specIndex, const Vec3& pos, const Vec3& rot, const Vec3& scale) {
    for (GameObject& o : objects_)
        if (o.specIndex == specIndex) {
            o.transform.position = pos;
            o.transform.rotation = rot;
            o.transform.scale = scale;
            o.prevRotY = rot.y;  // renderEditor без интерполяции, но держим согласованным
            return;
        }
}

bool Scene::editorWorldAABB(int specIndex, Vec3& mn, Vec3& mx) const {
    for (const GameObject& o : objects_)
        if (o.specIndex == specIndex) {
            worldAabb(o.transform.matrix(), o.aabbMin, o.aabbMax, mn, mx);
            return true;
        }
    return false;
}

int Scene::editorAddObjectModel(Renderer& renderer, AssetSource& assets, const std::string& model,
                                const std::string& tex, ShaderType shader, const Vec3& pos,
                                int specIndex) {
    MeshData md;
    TextureData embedded;
    bool hasTex = false;
    if (!loadGltfStatic(assets, model.c_str(), md, embedded, hasTex)) {
        LOGW("Editor: модель %s не загрузилась — объект не добавлен", model.c_str());
        return -1;
    }
    GameObject o;
    o.mesh = renderer.createMesh(md);
    TextureHandle t = 0;
    if (!tex.empty()) {
        TextureData ext;
        if (loadImageAsset(assets, tex.c_str(), ext)) t = renderer.createTexture(ext);
    } else if (hasTex) {
        t = renderer.createTexture(embedded);
    }
    MaterialDesc mat;
    mat.shader = shader;
    mat.baseColor = {1.0f, 1.0f, 1.0f};
    mat.albedo = t;
    o.material = renderer.createMaterial(mat);
    o.albedo = t;  // для последующей смены shader/color в редакторе
    aabbOfMesh(md, o.aabbMin, o.aabbMax);
    o.transform.position = pos;
    o.transform.scale = {1.0f, 1.0f, 1.0f};
    o.specIndex = specIndex;
    objects_.push_back(o);
    return specIndex;
}

int Scene::editorDuplicateObject(int srcSpecIndex, const Vec3& pos, int newSpecIndex) {
    for (size_t i = 0; i < objects_.size(); ++i) {
        if (objects_[i].specIndex != srcSpecIndex) continue;
        GameObject o = objects_[i];  // POD-копия: хендлы меша/материала/альбедо шарятся с оригиналом
        o.transform.position = pos;
        o.specIndex = newSpecIndex;
        objects_.push_back(o);
        return newSpecIndex;
    }
    return -1;
}

void Scene::editorSetObjectMaterial(Renderer& renderer, int specIndex, ShaderType shader,
                                    const Vec3& color) {
    for (GameObject& o : objects_)
        if (o.specIndex == specIndex) {
            MaterialDesc mat;
            mat.shader = shader;
            mat.baseColor = color;
            mat.albedo = o.albedo;  // сохраняем текстуру объекта, меняем только shader/тинт
            o.material = renderer.createMaterial(mat);
            return;
        }
}

// --- Здания ---
namespace {
// Центр и радиус пикинга здания i (из визуала; фолбэк, если pickRadius не задан).
float buildingPickRadius(const EntityVisual& v) { return v.pickRadius > 0.1f ? v.pickRadius : 1.2f; }
}  // namespace

bool Scene::editorPickBuilding(const Vec3& ro, const Vec3& rd, int& idx, float& t) const {
    int best = -1;
    float bestT = 1e30f;
    for (size_t i = 0; i < sceneDesc_.buildings.size(); ++i) {
        const BuildingSpec& b = sceneDesc_.buildings[i];
        const EntityVisual& v = visual(buildingType(b.kind));
        Vec3 c = b.pos + Vec3{0.0f, v.yOffset, 0.0f};
        float th = raySphere(ro, rd, c, buildingPickRadius(v));
        if (th >= 0.0f && th < bestT) { bestT = th; best = (int)i; }
    }
    idx = best;
    t = bestT;
    return best >= 0;
}

bool Scene::editorBuildingInfo(int i, int& type, int& team, Vec3& pos) const {
    if (i < 0 || i >= (int)sceneDesc_.buildings.size()) return false;
    const BuildingSpec& b = sceneDesc_.buildings[i];
    type = (int)buildingType(b.kind);
    team = (int)b.team;
    pos = b.pos;
    return true;
}

void Scene::editorSetBuildingPos(int i, const Vec3& pos) {
    if (i >= 0 && i < (int)sceneDesc_.buildings.size()) sceneDesc_.buildings[i].pos = pos;
}

void Scene::editorSetBuildingTeam(int i, int team) {
    if (i >= 0 && i < (int)sceneDesc_.buildings.size()) sceneDesc_.buildings[i].team = (uint8_t)team;
}

bool Scene::editorBuildingWorldAABB(int i, Vec3& mn, Vec3& mx) const {
    if (i < 0 || i >= (int)sceneDesc_.buildings.size()) return false;
    const BuildingSpec& b = sceneDesc_.buildings[i];
    const EntityVisual& v = visual(buildingType(b.kind));
    float r = buildingPickRadius(v);
    Vec3 c = b.pos + Vec3{0.0f, v.yOffset, 0.0f};
    mn = c - Vec3{r, r, r};
    mx = c + Vec3{r, r, r};
    return true;
}

int Scene::editorAddBuilding(int type, const Vec3& pos, int team) {
    BuildingSpec b;
    b.kind = buildingKind(type);
    b.pos = pos;
    b.team = (uint8_t)team;
    sceneDesc_.buildings.push_back(b);
    return (int)sceneDesc_.buildings.size() - 1;
}

void Scene::editorRemoveBuilding(int i) {
    if (i >= 0 && i < (int)sceneDesc_.buildings.size())
        sceneDesc_.buildings.erase(sceneDesc_.buildings.begin() + i);
}

// --- Точки спавна ---
bool Scene::editorPickSpawn(const Vec3& ro, const Vec3& rd, int& idx, float& t) const {
    int best = -1;
    float bestT = 1e30f;
    for (size_t i = 0; i < sceneDesc_.spawns.size(); ++i) {
        Vec3 c = sceneDesc_.spawns[i].pos + Vec3{0.0f, 0.4f, 0.0f};
        float th = raySphere(ro, rd, c, 0.7f);
        if (th >= 0.0f && th < bestT) { bestT = th; best = (int)i; }
    }
    idx = best;
    t = bestT;
    return best >= 0;
}

bool Scene::editorSpawnInfo(int i, int& team, Vec3& pos) const {
    if (i < 0 || i >= (int)sceneDesc_.spawns.size()) return false;
    team = (int)sceneDesc_.spawns[i].team;
    pos = sceneDesc_.spawns[i].pos;
    return true;
}

void Scene::editorSetSpawnPos(int i, const Vec3& pos) {
    if (i >= 0 && i < (int)sceneDesc_.spawns.size()) sceneDesc_.spawns[i].pos = pos;
}

void Scene::editorSetSpawnTeam(int i, int team) {
    if (i >= 0 && i < (int)sceneDesc_.spawns.size()) sceneDesc_.spawns[i].team = (uint8_t)team;
}

bool Scene::editorSpawnWorldAABB(int i, Vec3& mn, Vec3& mx) const {
    if (i < 0 || i >= (int)sceneDesc_.spawns.size()) return false;
    Vec3 c = sceneDesc_.spawns[i].pos + Vec3{0.0f, 0.4f, 0.0f};
    mn = c - Vec3{0.6f, 0.6f, 0.6f};
    mx = c + Vec3{0.6f, 0.6f, 0.6f};
    return true;
}

int Scene::editorAddSpawn(const Vec3& pos, int team) {
    SpawnSpec s;
    s.pos = pos;
    s.team = (uint8_t)team;
    sceneDesc_.spawns.push_back(s);
    return (int)sceneDesc_.spawns.size() - 1;
}

void Scene::editorRemoveSpawn(int i) {
    if (i >= 0 && i < (int)sceneDesc_.spawns.size())
        sceneDesc_.spawns.erase(sceneDesc_.spawns.begin() + i);
}

void Scene::editorRemoveObject(int specIndex) {
    objects_.erase(std::remove_if(objects_.begin(), objects_.end(),
                                  [&](const GameObject& o) { return o.specIndex == specIndex; }),
                   objects_.end());
    for (GameObject& o : objects_)
        if (o.specIndex > specIndex) o.specIndex--;  // держим specIndex == индексу в doc.objects
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

    // Отправляем ввод серверу и запоминаем его как неподтверждённый (для реплея реконсиляции).
    if (session_.connected()) {
        session_.sendInput(cmd);       // проставляет cmd.seq
        clientWorld_.recordInput(cmd, dt);  // окно неподтверждённых ограничено (P2-03)
    }

    player_.snapshot();                        // зафиксировать прошлое для интерполяции
    player_.simulate(dt, cmd, clientWorld_.collision());  // локальное предсказание (с коллизиями)
    player_.animTime += dt;  // фаза анимации — ровно 1 раз за тик (не в simulate: реплей
                             // реконсиляции зовёт simulate многократно и ускорял бы её)
    player_.locoPhase += dt * locoRate(chars_, localCharIndex_, player_.animParam);  // фаза локомоции
    if (player_.attackTime > 0.0f) {  // отсчёт каста — тоже 1 раз/тик (триггер — в simulate)
        player_.attackTime -= dt;
        if (player_.attackTime < 0.0f) player_.attackTime = 0.0f;
    }

    // Пампинг сети (host -> server poll/tick; всегда client poll) + приём снапшотов ->
    // реконсиляция своего аватара + буфер чужих (ClientWorld). navDebug держит Scene.
    if (session_.pump(dt)) {
        clientWorld_.applySnapshot(player_, remoteEntities_, session_, presentation_, mobs_,
                                   localTeam_, localHp_, localRespawn_);
        if (navDebug_) rebuildNavDebug();
    }

    // Реакция на обрыв. Чужие сущности застыли бы на последнем снапшоте — чистим их
    // (и неподтверждённые вводы), чтобы на экране не висели «призраки», затем сессия сама
    // пробует переподключиться (для join; host к 127.0.0.1 не трогает).
    if (session_.status() == NetStatus::Lost) {
        if (!remoteEntities_.empty()) remoteEntities_.clear();
        if (!presentation_.dyingMobs.empty()) presentation_.dyingMobs.clear();
        clientWorld_.clearPending();
        session_.reconnectTick(dt);
    }

    clientWorld_.advanceClock(dt);
}

// --- Читаемость боя + джус (клиентская косметика). Константы/предикаты (kFlashDur/…/isCombatType/
// combatHeadHeight) — в game/CombatFx.h (общие с ClientWorld::applySnapshot). Здесь — рендер-хелперы. ---
// Подмешать «горячий» цвет вспышки к базовому по t∈[0..1] (overbright — uColor>1 высветляет).
static Vec3 flashMix(Vec3 base, float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    const Vec3 hot{2.4f, 2.0f, 1.7f};
    return base + (hot - base) * t;
}
// Применить hit-fx к скиннинг-модели: белая вспышка + scale-punch (t = flash/kFlashDur).
static void applyHitFx(SkinnedItem& it, float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    it.color = flashMix(it.color, t);
    const float s = 1.0f + kPunch * t;                 // короткий «тычок» масштаба (вокруг ног)
    it.model = it.model * Mat4::scale({s, s, s});
}



void Scene::setNavDebugEnabled(bool e) {
    navDebug_ = e;
    if (e) {
        navDebugSig_ = 0;             // форсируем пересборку при следующем вызове
        navDebugFrame_.valid = false;
        rebuildNavDebug();            // показать сразу, если сессия уже идёт
    }
}

void Scene::rebuildNavDebug() {
    if (!navDebug_) return;
    // Подпись входа: набор блокирующих зданий/ядер (id + тип + жив + позиция). Здания
    // статичны, так что поле меняется лишь на постройке/сносе — без изменений пропускаем
    // O(N) BFS (важно на полях в сотни клеток).
    uint64_t sig = 0;
    for (const RemoteEntity& r : remoteEntities_) {
        const EntityType t = (EntityType)r.type;
        const bool relevant = blocksPath(t) || t == EntityType::Core;
        if (!relevant) continue;
        const uint64_t hpBit = (r.hp > 0.0f) ? 1ull : 0ull;
        const uint64_t px = (uint64_t)(int64_t)std::lround(r.ch.position.x * 4.0f) & 0xFFFFull;
        const uint64_t pz = (uint64_t)(int64_t)std::lround(r.ch.position.z * 4.0f) & 0xFFFFull;
        sig ^= ((uint64_t)r.id * 0x9E3779B97F4A7C15ull) ^ (hpBit << 39) ^ (px << 8) ^
               (pz << 24) ^ ((uint64_t)r.type << 56);
    }
    if (navDebugFrame_.valid && sig == navDebugSig_) return;  // ничего не изменилось
    navDebugSig_ = sig;

    // Те же входы, что у сервера: коллайдеры сцены + футпринты блокирующих зданий; цели —
    // живые ядра (основная цель мобов, GameWorld::ensureFlowField.toCore). Предикат blocksPath
    // и геометрия footprintBox — общие с сервером (game/BuildRules.h).
    std::vector<NavDebugBlocker> blockers;
    std::vector<NavCell> goals;
    for (const RemoteEntity& r : remoteEntities_) {
        const EntityType t = (EntityType)r.type;
        if (blocksPath(t)) {
            Vec3 c, hf;
            footprintBox(r.ch.position, grid_.cell, c, hf);
            blockers.push_back({c, hf});
        }
        if (t == EntityType::Core && r.hp > 0.0f) {
            goals.push_back({grid_.cellOf(r.ch.position.x), grid_.cellOf(r.ch.position.z)});
        }
    }
    // clearance = серверный kEnemyRadius (0.3): клетка, куда капсула моба не влезет, — занята.
    navDebugFrame_ = buildNavDebug(grid_, sceneDesc_.colliders, blockers, goals, 0.3f);
}

void Scene::hostGame() {
    leaveGame();               // сброс мира + прошлой сессии
    session_.host(sceneDesc_);  // поднять локальный сервер той же геометрией + подключиться
}

void Scene::joinGame(const char* ip, uint16_t port) {
    leaveGame();
    session_.join(ip, port);
}

void Scene::leaveGame() {
    session_.leave();  // транспорт: disconnect + stop host-сервера + выкл. реконнект
    // Мировое состояние (сессия его не знает) чистит Scene.
    remoteEntities_.clear();
    clientWorld_.syncBuildingColliders(remoteEntities_);  // снять все футпринт-боксы (список пуст)
    clientWorld_.clearPending();
    presentation_.dyingMobs.clear();
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
    // Часы боя: тикают, пока идёт бой (подключены, фаза Playing); замерзают на исходе; сбрасываются
    // при переходе исход->Playing (рестарт). Вход в бой сбрасывает извне (resetMatchClock).
    {
        int ph = matchPhase();
        if (ph == 0 && prevPhase_ != 0) matchClock_ = 0.0f;   // рестарт матча в той же сессии
        if (ph == 0 && session_.connected()) matchClock_ += renderDt;
        prevPhase_ = ph;
    }

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
    // Цвет горизонта (sRGB->линейный): фон-очистка + цель тинта дальней земли (шейдер Ground).
    frame.fogColor = {std::pow(sceneDesc_.horizonColor.x, 2.2f),
                      std::pow(sceneDesc_.horizonColor.y, 2.2f),
                      std::pow(sceneDesc_.horizonColor.z, 2.2f)};
    frame.fogDensity = 0.0f;  // объёмного тумана нет — тинт земли делает шейдер по дистанции

    // Джус: тряска камеры на крупный урон — затухающий сдвиг в экранной плоскости (нудж по
    // translation view-матрицы). Применяем ДО сохранения presentation_.lastView, чтобы HUD-оверлей (бары/
    // числа/искры) трясся ВМЕСТЕ с миром, а не разъезжался с ним.
    if (presentation_.shakeTime > 0.0f) {
        presentation_.shakeTime = (presentation_.shakeTime > renderDt) ? presentation_.shakeTime - renderDt : 0.0f;
        float env = presentation_.shakeTime / kShakeDur;          // 1 -> 0
        float amp = presentation_.shakeAmp * env * env;           // квадратичный спад — резче гаснет
        float ph = kShakeDur - presentation_.shakeTime;           // растущая фаза
        frame.view.m[12] += amp * std::sin(ph * 78.0f);        // гориз. дрожь
        frame.view.m[13] += amp * std::sin(ph * 61.0f + 1.7f); // верт. дрожь (иная частота)
        if (presentation_.shakeTime <= 0.0f) presentation_.shakeAmp = 0.0f;
    }

    // Читаемость боя: матрицы кадра для проекции маркеров (GameUi рисует бары/числа/искры),
    // сброс списка маркеров (пересобираем ниже) и продвижение косметических таймеров.
    presentation_.lastView = frame.view;
    presentation_.lastProj = frame.proj;
    presentation_.markers.clear();
    if (presentation_.localFlash > 0.0f) presentation_.localFlash = (presentation_.localFlash > renderDt) ? presentation_.localFlash - renderDt : 0.0f;
    for (auto it = presentation_.flash.begin(); it != presentation_.flash.end();) {
        it->second -= renderDt;
        if (it->second <= 0.0f) it = presentation_.flash.erase(it);
        else ++it;
    }
    for (size_t i = 0; i < presentation_.damageNumbers.size();) {
        presentation_.damageNumbers[i].age += renderDt;
        if (presentation_.damageNumbers[i].age >= kDmgLife) presentation_.damageNumbers.erase(presentation_.damageNumbers.begin() + (long)i);
        else ++i;
    }
    for (size_t i = 0; i < presentation_.sparks.size();) {
        presentation_.sparks[i].age += renderDt;
        if (presentation_.sparks[i].age >= presentation_.sparks[i].maxAge) presentation_.sparks.erase(presentation_.sparks.begin() + (long)i);
        else ++i;
    }
    for (size_t i = 0; i < presentation_.poofs.size();) {
        presentation_.poofs[i].age += renderDt;
        if (presentation_.poofs[i].age >= kPoofLife) presentation_.poofs.erase(presentation_.poofs.begin() + (long)i);
        else ++i;
    }

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
        float lp = player_.prevLocoPhase + (player_.locoPhase - player_.prevLocoPhase) * alpha;
        float atk = player_.prevAttackTime + (player_.attackTime - player_.prevAttackTime) * alpha;
        SkinnedItem hi = makeSkinnedItem(chars_, localCharIndex_, p, yaw, ap, at, atk, -1, 0.0f, lp);
        if (presentation_.localFlash > 0.0f) applyHitFx(hi, presentation_.localFlash / kFlashDur);  // вспышка + punch при уроне
        frame.skinned.push_back(hi);
    }

    // Удалённые игроки: рендерим «прошлое» на kInterpDelay назад, интерполируя
    // между двумя снапшотами из буфера. Это и есть snapshot interpolation.
    const double kInterpDelay = 0.1;  // сек буфера — гасит джиттер/потери
    double renderTime = clientWorld_.simClock() + (double)(alpha * tickDt_) - kInterpDelay;
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
            r.ch.locoPhase += renderDt * locoRate(chars_, r.charType, r.ch.animParam);  // фаза локомоции
            SkinnedItem it =
                makeSkinnedItem(chars_, r.charType, r.ch.position, r.ch.facingYaw, r.ch.animParam,
                                r.ch.animTime, r.ch.attackTime, -1, 0.0f, r.ch.locoPhase);
            it.color = (r.team == localTeam_) ? Vec3{0.55f, 0.75f, 1.0f}   // союзник
                                              : Vec3{1.0f, 0.45f, 0.45f};  // враг
            auto fh = presentation_.flash.find(r.id);  // вспышка + punch при уроне поверх командного оттенка
            if (fh != presentation_.flash.end()) applyHitFx(it, fh->second / kFlashDur);
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
            auto fe = presentation_.flash.find(r.id);  // вспышка + punch врага при попадании
            if (fe != presentation_.flash.end()) applyHitFx(it, fe->second / kFlashDur);
            frame.skinned.push_back(it);
        } else if (et == EntityType::Projectile && projMesh_ != 0) {
            // Снаряд (серверная сущность): тонкий вытянутый болт вдоль полёта (yaw с сервера).
            // Цвет — по виду башни (charType); снаряд героя (0xFF) -> базовый жёлтый болт.
            const int pk = (int)r.charType;
            MaterialHandle pmat = (pk >= 0 && pk < (int)projKindMat_.size()) ? projKindMat_[pk] : projMat_;
            Mat4 m = Mat4::translation(r.ch.position) * Mat4::rotationY(r.ch.facingYaw) *
                     Mat4::scale({0.08f, 0.08f, 0.6f});
            frame.items.push_back({projMesh_, pmat, m});
        } else if (et == EntityType::Tower) {
            // Башня: пилон, крашеный тинтом вида (kind в младшем ниббле charType), масштаб растёт
            // с тиром (старший ниббл). Откат на базовый материал/меш, если ростер/тинт не собран.
            const int kind = r.charType & 0x0F;
            int tier = (r.charType >> 4); if (tier < 1) tier = 1;
            const EntityVisual& v = visual(EntityType::Tower);
            if (v.mesh != 0) {
                MaterialHandle mat = (kind < (int)towerKindMat_.size() && towerKindMat_[kind] != 0)
                                         ? towerKindMat_[kind] : v.material;
                const float s = 1.0f + 0.12f * (float)(tier - 1);
                frame.items.push_back({v.mesh, mat,
                    Mat4::translation(r.ch.position + Vec3{0.0f, v.yOffset, 0.0f}) *
                        Mat4::scale({s, s, s})});
            }
        } else if (et == EntityType::Trap && trapMesh_ != 0) {
            // Ловушка: низкая широкая плита на полу, цвет вида; тир слегка увеличивает.
            const int kind = r.charType & 0x0F;
            int tier = (r.charType >> 4); if (tier < 1) tier = 1;
            MaterialHandle mat = (kind < (int)towerKindMat_.size() && towerKindMat_[kind] != 0)
                                     ? towerKindMat_[kind] : projMat_;
            const float w = grid_.cell * 0.72f * (1.0f + 0.12f * (float)(tier - 1));
            frame.items.push_back({trapMesh_, mat,
                Mat4::translation(r.ch.position + Vec3{0.0f, 0.11f, 0.0f}) *
                    Mat4::scale({w, 0.2f, w})});
        } else {
            const EntityVisual& v = visual(et);
            if (v.mesh != 0)
                frame.items.push_back({v.mesh, v.material,
                    Mat4::translation(r.ch.position + Vec3{0.0f, v.yOffset, 0.0f})});
        }

        // HP-бар над боевой сущностью (враг/герой/башня/ядро) — из интерполированной позиции.
        if (isCombatType(et) && r.hp > 0.0f) {
            auto mit = presentation_.maxHpSeen.find(r.id);
            float mx = (mit != presentation_.maxHpSeen.end() && mit->second > 0.0f) ? mit->second : r.hp;
            float frac = r.hp / mx;
            if (frac < 0.0f) frac = 0.0f;
            if (frac > 1.0f) frac = 1.0f;
            Vec3 bcol = (et == EntityType::Enemy)  ? Vec3{0.90f, 0.25f, 0.20f}   // враг — красный
                        : (r.team == localTeam_)   ? Vec3{0.40f, 0.85f, 0.40f}   // свой/союзник — зелёный
                                                   : Vec3{0.90f, 0.25f, 0.20f};  // враг-сторона (PvP) — красный
            presentation_.markers.push_back({r.ch.position + Vec3{0.0f, combatHeadHeight(et), 0.0f}, frac, bcol});
        }
    }

    // «Трупы» убитых мобов: клип смерти на месте гибели, затем убираем. Чисто клиентская
    // косметика (сервер сущность уже удалил) — заводится в applySnapshot.
    for (size_t i = 0; i < presentation_.dyingMobs.size();) {
        DyingMob& d = presentation_.dyingMobs[i];
        d.t += renderDt;
        if (d.t >= d.dur) { presentation_.dyingMobs.erase(presentation_.dyingMobs.begin() + (long)i); continue; }
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
        // Призрак типа на целевой клетке, зелёный/красный. Ловушка — плита (не в visuals_).
        MaterialHandle gmat = valid ? ghostOkMat_ : ghostBadMat_;
        if (buildType_ == EntityType::Trap && trapMesh_ != 0) {
            const float w = grid_.cell * 0.72f;
            frame.items.push_back({trapMesh_, gmat,
                Mat4::translation(tcenter + Vec3{0.0f, 0.11f, 0.0f}) * Mat4::scale({w, 0.2f, w})});
        } else {
            const EntityVisual& v = visual(buildType_);
            if (v.mesh != 0)
                frame.items.push_back({v.mesh, gmat,
                    Mat4::translation(tcenter + Vec3{0.0f, v.yOffset, 0.0f})});
        }
    }

    // HUD ставок героя (bitmap-шрифт — только ASCII; кириллический баннер — в ImGui-слое
    // через геттеры heroDead()/heroRespawnLeft()). Верхний левый угол, под FPS.
    if (session_.connected()) {
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

int Scene::matchPhase() const { return (int)session_.gamePhase(); }

int Scene::enemyCount() const {
    int n = 0;
    for (const RemoteEntity& r : remoteEntities_)
        if ((EntityType)r.type == EntityType::Enemy) ++n;
    return n;
}

float Scene::coreHp() const {
    for (const RemoteEntity& r : remoteEntities_)
        if ((EntityType)r.type == EntityType::Core) return r.hp;
    return -1.0f;  // ядра нет в снапшотах (не подключены / соло без сети)
}

float Scene::coreMaxHp() const { return config_.get(EntityType::Core).hp; }

Scene::SceneBriefing Scene::sceneBriefing() const {
    // Считаем из сохранённого описания сцены (sceneDesc_), которым уже построен мир.
    SceneBriefing b;
    int nonzeroTeam = 0;   // сколько РАЗНЫХ ненулевых команд встретили (>=2 -> PvP)
    uint8_t firstTeam = 0;
    auto noteTeam = [&](uint8_t t) {
        if (t == 0) return;
        if (nonzeroTeam == 0) { firstTeam = t; nonzeroTeam = 1; }
        else if (t != firstTeam) nonzeroTeam = 2;
    };
    for (const BuildingSpec& bs : sceneDesc_.buildings) {
        noteTeam(bs.team);
        if (bs.kind == BuildingSpec::Core) {
            b.hasCore = true;
            b.coreHp += bs.hp;
        } else if (bs.kind == BuildingSpec::Spawner) {
            b.spawnerCount++;
            if (bs.waveSize > 0) {
                b.infiniteWaves = true;
                b.waveBase += bs.waveSize;
                b.waveGrow += bs.waveGrow;
            }
        }
    }
    for (const SpawnSpec& sp : sceneDesc_.spawns) noteTeam(sp.team);
    b.pvp = (nonzeroTeam >= 2);
    return b;
}

Scene::Minimap Scene::sceneMinimap() const {
    Minimap md;
    float minX = 1e9f, minZ = 1e9f, maxX = -1e9f, maxZ = -1e9f;
    auto expand = [&](float x, float z) {
        if (x < minX) minX = x;
        if (x > maxX) maxX = x;
        if (z < minZ) minZ = z;
        if (z > maxZ) maxZ = z;
    };
    // Колайдеры: пол исключаем (его верх у земли, ~0); стены/препятствия выше.
    for (const ColliderSpec& c : sceneDesc_.colliders) {
        if (c.center.y + c.half.y <= 0.3f) continue;  // пол/приземные плиты — не рисуем
        md.walls.push_back({c.center.x, c.center.z, c.half.x, c.half.z});
        expand(c.center.x - c.half.x, c.center.z - c.half.z);
        expand(c.center.x + c.half.x, c.center.z + c.half.z);
    }
    // Точки интереса: ядро, спавнеры врагов.
    for (const BuildingSpec& bs : sceneDesc_.buildings) {
        if (bs.kind == BuildingSpec::Core) {
            md.marks.push_back({bs.pos.x, bs.pos.z, Minimap::Poi::Core});
            expand(bs.pos.x, bs.pos.z);
        } else if (bs.kind == BuildingSpec::Spawner) {
            md.marks.push_back({bs.pos.x, bs.pos.z, Minimap::Poi::Spawner});
            expand(bs.pos.x, bs.pos.z);
        }
    }
    // Старт героя: точки спавна команд (PvP), иначе позиция игрока из сцены.
    if (!sceneDesc_.spawns.empty()) {
        for (const SpawnSpec& s : sceneDesc_.spawns) {
            md.marks.push_back({s.pos.x, s.pos.z, Minimap::Poi::HeroSpawn});
            expand(s.pos.x, s.pos.z);
        }
    } else if (sceneDesc_.player.present) {
        md.marks.push_back({sceneDesc_.player.pos.x, sceneDesc_.player.pos.z, Minimap::Poi::HeroSpawn});
        expand(sceneDesc_.player.pos.x, sceneDesc_.player.pos.z);
    }
    if (minX > maxX) {  // ничего не нашли — фолбэк на арену из грида (квадрат)
        float a = sceneDesc_.grid.arenaHalf > 0.0f ? sceneDesc_.grid.arenaHalf : 12.0f;
        minX = minZ = -a;
        maxX = maxZ = a;
    }
    md.minX = minX;
    md.minZ = minZ;
    md.maxX = maxX;
    md.maxZ = maxZ;
    md.valid = true;
    return md;
}

const EntityVisual& Scene::visual(EntityType t) const {
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
        // Ловушка не в таблице визуалов (рисуется своей плитой) — пикаем её сферой над клеткой.
        float pickRadius, yOff;
        if ((EntityType)r.type == EntityType::Trap) {
            pickRadius = grid_.cell * 0.6f;
            yOff = 0.2f;
        } else {
            const EntityVisual& v = visual((EntityType)r.type);
            if (!v.pickable) continue;
            pickRadius = v.pickRadius;
            yOff = v.yOffset;
        }
        Vec3 c = r.ch.position + Vec3{0.0f, yOff, 0.0f};
        Vec3 oc = origin - c;
        float b = dot(oc, dir);
        float cc = dot(oc, oc) - pickRadius * pickRadius;
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
    if (t < 0 || t >= 8) return nullptr;  // Trap (8) — не в BuildingConfig (см. selectedDefense*)
    const BuildingInfo& bi = config_.get((EntityType)t);
    return bi.defined ? &bi : nullptr;
}

float Scene::selectedAux() const {
    for (const RemoteEntity& r : remoteEntities_)
        if (r.id == selectedId_) return r.aux;
    return 0.0f;
}

bool Scene::selectedWorldPos(Vec3& out) const {
    if (selectedId_ == 0) return false;
    for (const RemoteEntity& r : remoteEntities_)
        if (r.id == selectedId_) {  // центр объекта (позиция + подъём визуала) для ринга
            out = r.ch.position + Vec3{0.0f, visual((EntityType)r.type).yOffset, 0.0f};
            return true;
        }
    return false;  // выделение снято или сущность исчезла (напр. разрушена)
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
    // Цена: защита (Tower/Trap) — из ростера towers.cfg по виду; прочее — из BuildingConfig.
    float cost;
    if (buildType_ == EntityType::Tower || buildType_ == EntityType::Trap) {
        if (buildKind_ < 0 || buildKind_ >= (int)sceneDesc_.towerTypes.size()) return false;
        cost = sceneDesc_.towerTypes[buildKind_].cost;
    } else {
        cost = config_.get(buildType_).cost;
    }
    if (resourceCurrent() < cost) return false;           // не хватает ресурса
    return true;
}

bool Scene::cellOccupied(int cx, int cz) const {
    for (const RemoteEntity& r : remoteEntities_) {
        if (!isBuildingType((EntityType)r.type)) continue;  // враг/снаряд не занимают клетку (Trap — да)
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

void Scene::beginBuild(int type, int kind) {
    buildType_ = (EntityType)type;
    buildKind_ = kind;  // вид защиты (для Tower/Trap); иначе не используется
    buildActive_ = true;
    clearSelection();
}

void Scene::confirmBuild() {
    if (!buildActive_ || !session_.connected()) return;
    int cx, cz;
    Vec3 center;
    if (!computeGhost(cx, cz, center)) return;  // невалидно — не шлём запрос
    session_.sendBuild((uint8_t)buildType_, (uint8_t)buildKind_, cx, cz);
    presentation_.emitSound(SoundId::Build);          // оптимистично: шлём только на валидной клетке
    presentation_.poofs.push_back({center, 0.0f});   // «пуф» размещения на центре клетки (косметика)
    // Остаёмся в режиме — можно ставить дальше (сервер авторитетно применит/отвергнет).
}

// --- Выделенная защита: апгрейд / снос ------------------------------------------------------
bool Scene::selectedIsDefense() const {
    int t = selectedEntityType();
    return t == (int)EntityType::Tower || t == (int)EntityType::Trap;
}

bool Scene::selectedMine() const {
    for (const RemoteEntity& r : remoteEntities_)
        if (r.id == selectedId_) return r.team == localTeam_;
    return false;
}

const char* Scene::selectedDefenseName() const {
    if (!selectedIsDefense()) return "";
    int kind = -1;
    for (const RemoteEntity& r : remoteEntities_)
        if (r.id == selectedId_) { kind = r.charType & 0x0F; break; }
    if (kind < 0 || kind >= (int)sceneDesc_.towerTypes.size()) return "";
    return sceneDesc_.towerTypes[kind].name.c_str();
}

int Scene::selectedTier() const {
    if (!selectedIsDefense()) return 0;
    for (const RemoteEntity& r : remoteEntities_)
        if (r.id == selectedId_) { int tr = (r.charType >> 4); return tr < 1 ? 1 : tr; }
    return 0;
}

int Scene::selectedMaxTier() const {
    if (!selectedIsDefense()) return 0;
    for (const RemoteEntity& r : remoteEntities_)
        if (r.id == selectedId_) {
            int kind = r.charType & 0x0F;
            if (kind >= 0 && kind < (int)sceneDesc_.towerTypes.size())
                return sceneDesc_.towerTypes[kind].maxTier;
        }
    return 0;
}

float Scene::selectedUpgradeCost() const {
    if (!selectedIsDefense()) return 0.0f;
    for (const RemoteEntity& r : remoteEntities_)
        if (r.id == selectedId_) {
            int kind = r.charType & 0x0F;
            int tier = (r.charType >> 4); if (tier < 1) tier = 1;
            if (kind < 0 || kind >= (int)sceneDesc_.towerTypes.size()) return 0.0f;
            return towerUpgradeCost(sceneDesc_.towerTypes[kind], tier);
        }
    return 0.0f;
}

bool Scene::selectedCanUpgrade() const {
    if (!selectedMine()) return false;
    float c = selectedUpgradeCost();
    return c > 0.0f && resourceCurrent() >= c;
}

bool Scene::selectedDemolishable() const {
    if (!selectedMine()) return false;
    int t = selectedEntityType();
    return t == (int)EntityType::Tower || t == (int)EntityType::Trap ||
           t == (int)EntityType::Generator || t == (int)EntityType::Storage;
}

float Scene::selectedRefund() const {
    if (!selectedDemolishable()) return 0.0f;
    // Оценка вложенного (как на сервере spent): защита — cost + сумма апгрейдов до тира; прочее —
    // цена постройки. Возврат — kDemolishRefund (0.6). Совпадает с сервером при простой истории.
    constexpr float kRefund = 0.6f;
    int t = selectedEntityType();
    if (t == (int)EntityType::Tower || t == (int)EntityType::Trap) {
        int kind = -1, tier = 1;
        for (const RemoteEntity& r : remoteEntities_)
            if (r.id == selectedId_) { kind = r.charType & 0x0F; tier = (r.charType >> 4); break; }
        if (tier < 1) tier = 1;
        if (kind < 0 || kind >= (int)sceneDesc_.towerTypes.size()) return 0.0f;
        const TowerDesc& d = sceneDesc_.towerTypes[kind];
        float spent = d.cost;
        for (int tt = 1; tt < tier; ++tt) spent += towerUpgradeCost(d, tt);
        return spent * kRefund;
    }
    return config_.get((EntityType)t).cost * kRefund;
}

void Scene::upgradeSelected() {
    if (selectedId_ != 0 && session_.connected() && selectedIsDefense()) session_.sendUpgrade(selectedId_);
}

void Scene::demolishSelected() {
    if (selectedId_ != 0 && session_.connected() && selectedDemolishable()) session_.sendDemolish(selectedId_);
}

// --- Палитра защиты (ростер towers.cfg) -----------------------------------------------------
int Scene::defenseCount() const { return (int)sceneDesc_.towerTypes.size(); }

const char* Scene::defenseName(int kind) const {
    if (kind < 0 || kind >= (int)sceneDesc_.towerTypes.size()) return "";
    return sceneDesc_.towerTypes[kind].name.c_str();
}

int Scene::defenseCost(int kind) const {
    if (kind < 0 || kind >= (int)sceneDesc_.towerTypes.size()) return 0;
    return (int)sceneDesc_.towerTypes[kind].cost;
}

int Scene::defenseEntityType(int kind) const {
    if (kind < 0 || kind >= (int)sceneDesc_.towerTypes.size()) return -1;
    return (int)sceneDesc_.towerTypes[kind].entity;
}
