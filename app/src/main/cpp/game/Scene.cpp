#include "game/Scene.h"

#include <cmath>
#include <string>
#include <unordered_map>

#include "engine/assets/Assets.h"
#include "engine/physics/CollisionWorld.h"
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
    // Конфиг зданий (параметры + тексты панели). Параметры применяем к зданиям сцены —
    // единый источник настроек: сцена размещает, конфиг задаёт rate/cap/…
    loadBuildingConfig(assets, "config/buildings.cfg", config_);
    applyBuildingConfig(sceneDesc_, config_);

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

    // --- Визуалы зданий базы (клиентский рендер по типу сущности) ---
    // Здания приходят с сервера в снапшотах; клиент рисует их этими мешами по EntityType.
    genMesh_ = renderer.createMesh(makeCube(1.0f));
    storMesh_ = renderer.createMesh(makeCube(1.4f));
    {
        MaterialDesc md;
        md.shader = ShaderType::Lit;
        md.baseColor = {0.35f, 0.80f, 0.40f};  // генератор — зелёный
        genMat_ = renderer.createMaterial(md);
    }
    {
        MaterialDesc md;
        md.shader = ShaderType::Lit;
        md.baseColor = {0.35f, 0.55f, 0.95f};  // хранилище — синее
        storMat_ = renderer.createMaterial(md);
    }
    spawnMesh_ = renderer.createMesh(makeCube(1.2f));
    coreMesh_ = renderer.createMesh(makeSphere(1.0f, 16, 24));
    enemyMesh_ = renderer.createMesh(makeSphere(0.35f, 12, 16));
    {
        MaterialDesc md;
        md.shader = ShaderType::Lit;
        md.baseColor = {0.55f, 0.35f, 0.75f};  // спавнер — фиолетовый
        spawnMat_ = renderer.createMaterial(md);
    }
    {
        MaterialDesc md;
        md.shader = ShaderType::Phong;
        md.baseColor = {0.95f, 0.85f, 0.35f};  // ядро — золотое (с бликом)
        coreMat_ = renderer.createMaterial(md);
    }
    {
        MaterialDesc md;
        md.shader = ShaderType::Lit;
        md.baseColor = {0.85f, 0.25f, 0.25f};  // враг — красный
        enemyMat_ = renderer.createMaterial(md);
    }
    towerMesh_ = renderer.createMesh(makeCube(0.9f));
    {
        MaterialDesc md;
        md.shader = ShaderType::Phong;
        md.baseColor = {0.70f, 0.72f, 0.78f};  // башня — стальная (с бликом)
        towerMat_ = renderer.createMaterial(md);
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
    if (foxMesh_ != 0) {
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

        // Рендер по типу сущности (задел под остальные типы — враги/башни/ядро).
        switch ((EntityType)r.type) {
            case EntityType::Hero:
                frame.skinned.push_back(
                    makeFoxItem(r.ch.position, r.ch.facingYaw, r.ch.animParam, r.ch.animTime));
                break;
            case EntityType::Generator:
                frame.items.push_back(
                    {genMesh_, genMat_, Mat4::translation(r.ch.position + Vec3{0.0f, 0.5f, 0.0f})});
                break;
            case EntityType::Storage:
                frame.items.push_back(
                    {storMesh_, storMat_, Mat4::translation(r.ch.position + Vec3{0.0f, 0.7f, 0.0f})});
                break;
            case EntityType::Spawner:
                frame.items.push_back(
                    {spawnMesh_, spawnMat_, Mat4::translation(r.ch.position + Vec3{0.0f, 0.6f, 0.0f})});
                break;
            case EntityType::Core:
                frame.items.push_back(
                    {coreMesh_, coreMat_, Mat4::translation(r.ch.position + Vec3{0.0f, 1.0f, 0.0f})});
                break;
            case EntityType::Tower:
                frame.items.push_back(
                    {towerMesh_, towerMat_, Mat4::translation(r.ch.position + Vec3{0.0f, 0.9f, 0.0f})});
                break;
            case EntityType::Enemy:
                frame.items.push_back(
                    {enemyMesh_, enemyMat_, Mat4::translation(r.ch.position + Vec3{0.0f, 0.35f, 0.0f})});
                break;
            default:
                break;
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
    float sum = 0.0f;
    for (const RemoteEntity& r : remoteEntities_)
        if ((EntityType)r.type == EntityType::Storage) sum += r.aux;
    return sum;
}

float Scene::resourceCap() const {
    float sum = 0.0f;
    for (const BuildingSpec& b : sceneDesc_.buildings)
        if (b.kind == BuildingSpec::Storage) sum += b.cap;
    return sum;
}

int Scene::matchPhase() const { return (int)client_.gamePhase(); }

float Scene::coreHp() const {
    for (const RemoteEntity& r : remoteEntities_)
        if ((EntityType)r.type == EntityType::Core) return r.hp;
    return -1.0f;  // ядра нет в снапшотах (не подключены / соло без сети)
}

float Scene::coreMaxHp() const { return config_.get(EntityType::Core).hp; }

namespace {
// Сфера для пикинга по типу: смещение центра вверх (как в рендере) + радиус.
// Возвращает false для не выбираемых типов (герой и пр.).
bool pickBounds(EntityType t, float& yoff, float& rad) {
    switch (t) {
        case EntityType::Generator: yoff = 0.5f;  rad = 1.0f; return true;
        case EntityType::Storage:   yoff = 0.7f;  rad = 1.2f; return true;
        case EntityType::Spawner:   yoff = 0.6f;  rad = 1.1f; return true;
        case EntityType::Core:      yoff = 1.0f;  rad = 1.3f; return true;
        case EntityType::Tower:     yoff = 0.9f;  rad = 1.0f; return true;
        case EntityType::Enemy:     yoff = 0.35f; rad = 0.6f; return true;
        default: return false;
    }
}
}  // namespace

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
        float yoff, rad;
        if (!pickBounds((EntityType)r.type, yoff, rad)) continue;
        Vec3 c = r.ch.position + Vec3{0.0f, yoff, 0.0f};
        Vec3 oc = origin - c;
        float b = dot(oc, dir);
        float cc = dot(oc, oc) - rad * rad;
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
