#include "game/Scene.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>

#include "engine/assets/Assets.h"
#include "engine/physics/CollisionWorld.h"
#include "game/Grid.h"
#include "engine/core/Log.h"
#include "engine/core/Renderer.h"
#include "game/SceneLoader.h"

Scene::Scene() = default;
Scene::~Scene() = default;

void Scene::build(Renderer& renderer, AssetSource& assets, const char* scenePath) {
    objects_.clear();
    collision_ = std::make_unique<CollisionWorld>();  // свежий мир коллизий на каждую сборку

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
    for (int t = 0; t < kEntityVisualCount; ++t) {
        const BuildingInfo& bi = config_.byType[t];
        if (bi.shape == MeshShape::None) continue;  // визуал в конфиге не задан
        EntityVisual& v = visuals_[t];
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

    // --- Статичные коллайдеры физики (из описания сцены) ---
    for (const ColliderSpec& cs : desc.colliders) {
        collision_->addBox(cs.center, cs.half);
    }
    collision_->finalize();  // оптимизация broad-phase после всей статики
    LOGI("Физика: %d статичных коллайдеров", (int)desc.colliders.size());

    // --- Управляемый персонаж (glTF + скиннинг) ---
    if (desc.player.present) {
        if (loadGltfModel(assets, desc.player.model.c_str(), foxModel_)) {
            foxMesh_ = renderer.createSkinnedMesh(foxModel_);
            if (foxModel_.hasTexture) {
                foxTex_ = renderer.createTexture(foxModel_.baseColor);
            }
            foxScale_ = desc.player.scale;
            foxYawOffset_ = desc.player.yawOffset;
        } else {
            LOGW("Не удалось загрузить модель игрока: %s", desc.player.model.c_str());
        }
        // Позиция и кинематический контроллер — независимо от загрузки модели (это игра).
        player_.position = desc.player.pos;
        player_.collider = collision_->addCharacter(desc.player.pos,
                                                    desc.player.colliderRadius,
                                                    desc.player.colliderCylHalf);
        player_.snapshot();  // prev = curr, чтобы первый кадр не «прыгнул»
    }
}

SkinnedItem Scene::makeFoxItem(Vec3 pos, float yaw, float animParam, float animTime) const {
    SkinnedItem item;
    item.mesh = foxMesh_;
    item.texture = foxTex_;
    item.color = (foxTex_ != 0) ? Vec3{1.0f, 1.0f, 1.0f} : Vec3{0.85f, 0.5f, 0.25f};
    item.model = Mat4::translation(pos)
               * Mat4::rotationY(yaw + foxYawOffset_)
               * Mat4::scale({foxScale_, foxScale_, foxScale_});

    if (!foxModel_.animations.empty()) {
        int n = (int)foxModel_.animations.size();
        auto id = [n](int i) { return i < n ? i : n - 1; };
        if (animParam <= 0.01f) {
            foxModel_.sampleAnimation(id(0), animTime, item.joints);
        } else if (animParam <= 1.0f) {
            foxModel_.sampleBlend(id(0), animTime, id(1), animTime, animParam, item.joints);
        } else {
            foxModel_.sampleBlend(id(1), animTime, id(2), animTime, animParam - 1.0f, item.joints);
        }
    }
    return item;
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
    cmd.faceMove = iy >= 0.0f;                      // назад — пятимся
    cmd.magnitude = (iy < 0.0f) ? mag * 0.5f : mag;  // назад медленнее
    cmd.jump = jumpQueued_;                          // одноразовый прыжок
    jumpQueued_ = false;

    if (heroDead()) {  // повержен — ввод в ноль (сервер держит героя на месте до респауна)
        cmd.moveX = cmd.moveZ = 0.0f;
        cmd.magnitude = 0.0f;
        cmd.jump = false;
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
        r->aux = s.aux;  // ресурс в хранилище и т.п. (последнее значение)
        r->hp = s.hp;    // здоровье (ядро/враг)
        r->buffer.push_back({simClock_, {s.x, s.y, s.z}, s.yaw, s.animParam});
        // Ограничиваем историю (~1 сек), чтобы буфер не рос.
        while (r->buffer.size() > 2 && r->buffer[1].t < simClock_ - 1.0) {
            r->buffer.erase(r->buffer.begin());
        }
    }

    // Убрать исчезнувшие сущности (нет в текущем снапшоте).
    for (size_t i = 0; i < remoteEntities_.size();) {
        bool found = false;
        for (const EntityState& s : states) {
            if (s.id == remoteEntities_[i].id) { found = true; break; }
        }
        if (!found) {
            remoteEntities_.erase(remoteEntities_.begin() + (long)i);
        } else {
            ++i;
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
    client_.connect(ip, kNetPort);
    host_ = false;
    inputSeq_ = 0;
}

void Scene::leaveGame() {
    client_.disconnect();
    server_.stop();
    host_ = false;
    remoteEntities_.clear();
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

    for (const GameObject& obj : objects_) {
        Transform t = obj.transform;
        t.rotation.y = obj.prevRotY + (obj.transform.rotation.y - obj.prevRotY) * alpha;
        frame.items.push_back({obj.mesh, obj.material, t.matrix()});
    }
    if (foxMesh_ != 0 && !heroDead()) {  // повержённого героя не рисуем (появится на респауне)
        // Свой аватар: интерполяция prev -> current по alpha.
        Vec3 p = player_.prevPosition + (player_.position - player_.prevPosition) * alpha;
        float yaw = lerpAngle(player_.prevFacingYaw, player_.facingYaw, alpha);
        float ap = player_.prevAnimParam + (player_.animParam - player_.prevAnimParam) * alpha;
        float at = player_.prevAnimTime + (player_.animTime - player_.prevAnimTime) * alpha;
        frame.skinned.push_back(makeFoxItem(p, yaw, ap, at));
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
                        break;
                    }
                }
            }
            r.ch.position = st.pos;
            r.ch.facingYaw = st.yaw;
            r.ch.animParam = st.anim;
        }
        r.ch.animTime += renderDt;  // фаза анимации крутится локально

        // Рендер по типу: Hero — скиннинг-лиса (спец), остальные — generic-меш из таблицы визуалов.
        EntityType et = (EntityType)r.type;
        if (et == EntityType::Hero) {
            // Чужой герой: союзник (та же команда) — синеватый, противник (PvP) — красный.
            // Свой герой рисуется отдельно (player_) обычным цветом — так их не спутать.
            SkinnedItem it =
                makeFoxItem(r.ch.position, r.ch.facingYaw, r.ch.animParam, r.ch.animTime);
            it.color = (r.team == localTeam_) ? Vec3{0.55f, 0.75f, 1.0f}   // союзник
                                              : Vec3{1.0f, 0.45f, 0.45f};  // враг
            frame.skinned.push_back(it);
        } else {
            const EntityVisual& v = visual(et);
            if (v.mesh != 0)
                frame.items.push_back({v.mesh, v.material,
                    Mat4::translation(r.ch.position + Vec3{0.0f, v.yOffset, 0.0f})});
        }
    }

    // Призрак размещения (режим стройки): куб типа на клетке перед героем, зелёный/красный.
    if (buildActive_) {
        int cx, cz;
        Vec3 center;
        bool valid = computeGhost(cx, cz, center);
        const EntityVisual& v = visual(buildType_);
        frame.items.push_back({v.mesh, valid ? ghostOkMat_ : ghostBadMat_,
                               Mat4::translation(center + Vec3{0.0f, v.yOffset, 0.0f})});
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
    for (const RemoteEntity& r : remoteEntities_) {       // клетка занята зданием?
        if (!visual((EntityType)r.type).building) continue;
        if (grid_.cellOf(r.ch.position.x) == cx && grid_.cellOf(r.ch.position.z) == cz)
            return false;
    }
    if (resourceCurrent() < config_.get(buildType_).cost) return false;  // не хватает ресурса
    return true;
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
