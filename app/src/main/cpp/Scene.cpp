#include "Scene.h"

#include <cmath>

#include "Assets.h"
#include "Log.h"
#include "Renderer.h"

void Scene::build(Renderer& renderer, AAssetManager* assets) {
    // --- Текстуры ---
    TextureHandle checker = renderer.createTexture(makeCheckerboard(256, 8));
    TextureHandle crateTex = 0;
    {
        TextureData img;
        if (loadImageAsset(assets, "textures/crate.png", img)) {
            crateTex = renderer.createTexture(img);
        }
    }

    // --- Материалы (окружение) ---
    MaterialHandle floorMat  = renderer.createMaterial({ShaderType::Lit,   {1.0f, 1.0f, 1.0f}, checker});
    MaterialHandle crateMat  = renderer.createMaterial({ShaderType::Lit,   {1.0f, 1.0f, 1.0f}, crateTex});
    MaterialHandle redMat    = renderer.createMaterial({ShaderType::Phong, {0.90f, 0.35f, 0.30f}, 0});
    MaterialHandle flatGreen = renderer.createMaterial({ShaderType::Unlit, {0.30f, 0.85f, 0.45f}, 0});
    MaterialHandle blueMat   = renderer.createMaterial({ShaderType::Phong, {0.35f, 0.55f, 0.95f}, 0});
    MaterialHandle goldMat   = renderer.createMaterial({ShaderType::Phong, {0.95f, 0.80f, 0.25f}, 0});

    // --- Меши окружения ---
    MeshHandle plane = renderer.createMesh(makePlane(24.0f, 12.0f));
    MeshHandle cube = renderer.createMesh(makeCube(1.0f));
    MeshHandle sphere = renderer.createMesh(makeSphere(0.6f));

    objects_.clear();

    {  // пол (побольше — есть где ходить)
        GameObject ground;
        ground.mesh = plane;
        ground.material = floorMat;
        objects_.push_back(ground);
    }

    const MaterialHandle cubeMats[] = {crateMat, redMat, flatGreen};
    for (int i = 0; i < 3; ++i) {
        GameObject c;
        c.mesh = cube;
        c.material = cubeMats[i];
        c.transform.position = {-4.0f + (float)i * 4.0f, 0.5f, -4.0f};
        c.spin = 0.6f + 0.3f * (float)i;
        objects_.push_back(c);
    }
    const MaterialHandle sphereMats[] = {blueMat, goldMat, redMat};
    for (int i = 0; i < 3; ++i) {
        GameObject s;
        s.mesh = sphere;
        s.material = sphereMats[i];
        s.transform.position = {-4.0f + (float)i * 4.0f, 0.6f, 4.0f};
        objects_.push_back(s);
    }

    // Кольцо инстансных кубов (демонстрация инстансинга) — как декор по краю.
    MaterialHandle ringMat = renderer.createMaterial({ShaderType::Lit, {0.55f, 0.65f, 0.85f}, checker});
    const int ringCount = 48;
    const float pi = 3.14159265358979323846f;
    for (int i = 0; i < ringCount; ++i) {
        float a = 2.0f * pi * (float)i / (float)ringCount;
        GameObject c;
        c.mesh = cube;
        c.material = ringMat;
        c.transform.position = {std::cos(a) * 10.0f, 0.4f, std::sin(a) * 10.0f};
        c.transform.scale = {0.4f, 0.4f, 0.4f};
        c.spin = 1.2f;
        objects_.push_back(c);
    }

    // --- Управляемый персонаж: лиса (glTF + скиннинг) ---
    if (loadGltfModel(assets, "models/Fox.glb", foxModel_)) {
        player_.model = &foxModel_;
        player_.mesh = renderer.createSkinnedMesh(foxModel_);
        if (foxModel_.hasTexture) {
            player_.tex = renderer.createTexture(foxModel_.baseColor);
        }
        player_.position = {0.0f, 0.0f, 0.0f};
        player_.snapshot();  // prev = curr, чтобы первый кадр не «прыгнул»
    } else {
        LOGW("Не удалось загрузить Fox.glb");
    }
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
    InputCommand cmd;
    float mag = joystick_.mag;
    if (mag > 0.05f) {
        float cy = camera_.yaw;
        Vec3 fwd{std::sin(cy), 0.0f, std::cos(cy)};
        Vec3 right{-std::cos(cy), 0.0f, std::sin(cy)};  // экранный right = cross(fwd, up)
        Vec3 moveDir = normalize(fwd * joystick_.y + right * joystick_.x);
        cmd.moveX = moveDir.x;
        cmd.moveZ = moveDir.z;
    }
    cmd.faceMove = joystick_.y >= 0.0f;            // назад — пятимся
    cmd.magnitude = (joystick_.y < 0.0f) ? mag * 0.5f : mag;  // назад медленнее

    // Отправляем ввод серверу (если в сети). Позицию считает сервер, но локально
    // предсказываем — иначе движение ощущалось бы с задержкой.
    if (client_.connected()) {
        cmd.seq = ++inputSeq_;
        client_.sendInput(cmd);
    }

    player_.snapshot();          // зафиксировать прошлое для интерполяции
    player_.simulate(dt, cmd);   // локальное предсказание

    // Сервер (если хостим) — тем же кодом симуляции, затем рассылка снапшотов.
    if (host_) {
        server_.poll();
        server_.tick(dt);
    }
    // Приём: снапшоты -> удалённые игроки + мягкая коррекция своего аватара.
    client_.poll();
    if (client_.consumeSnapshot()) {
        applySnapshot();
    }
}

void Scene::applySnapshot() {
    const std::vector<EntityState>& states = client_.states();
    uint32_t myId = client_.myId();

    for (const EntityState& s : states) {
        if (s.id == myId) {
            // Мягкая сверка с сервером (без полного реплея — упрощённо).
            Vec3 sp{s.x, s.y, s.z};
            player_.position = player_.position + (sp - player_.position) * 0.15f;
            player_.facingYaw = lerpAngle(player_.facingYaw, s.yaw, 0.15f);
            continue;
        }
        RemotePlayer* r = nullptr;
        for (auto& rp : remotes_) {
            if (rp.id == s.id) { r = &rp; break; }
        }
        if (r == nullptr) {  // новый игрок — создаём с моделью лисы
            RemotePlayer rp;
            rp.id = s.id;
            rp.ch.model = &foxModel_;
            rp.ch.mesh = player_.mesh;
            rp.ch.tex = player_.tex;
            rp.ch.scale = player_.scale;
            rp.ch.modelYawOffset = player_.modelYawOffset;
            rp.ch.position = {s.x, s.y, s.z};
            rp.ch.facingYaw = s.yaw;
            remotes_.push_back(rp);
            r = &remotes_.back();
        }
        r->targetPos = {s.x, s.y, s.z};
        r->targetYaw = s.yaw;
        r->targetAnim = s.animParam;
    }

    // Убрать отключившихся (отсутствуют в снапшоте).
    for (size_t i = 0; i < remotes_.size();) {
        bool found = false;
        for (const EntityState& s : states) {
            if (s.id == remotes_[i].id) { found = true; break; }
        }
        if (!found) {
            remotes_.erase(remotes_.begin() + (long)i);
        } else {
            ++i;
        }
    }
}

void Scene::hostGame() {
    leaveGame();
    if (server_.start(kNetPort)) {
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
    remotes_.clear();
}

void Scene::onPointer(float x, float y, bool pressed) {
    joystick_.onPointer(x, y, pressed);
}

RenderFrame Scene::render(float alpha, float aspect, float renderDt) {
    // Камера следует за ИНТЕРПОЛИРОВАННОЙ позицией цели (плавно, на рендер-частоте).
    Vec3 focusPos = player_.prevPosition + (player_.position - player_.prevPosition) * alpha;
    float focusYaw = lerpAngle(player_.prevFacingYaw, player_.facingYaw, alpha);
    camera_.follow(focusPos, focusYaw, renderDt);

    RenderFrame frame;
    frame.view = camera_.view();
    frame.proj = camera_.proj(aspect);
    frame.cameraPos = camera_.eye();
    frame.lightDir = normalize(Vec3{0.4f, 1.0f, 0.6f});

    for (const GameObject& obj : objects_) {
        Transform t = obj.transform;
        t.rotation.y = obj.prevRotY + (obj.transform.rotation.y - obj.prevRotY) * alpha;
        frame.items.push_back({obj.mesh, obj.material, t.matrix()});
    }
    if (player_.mesh != 0) {
        frame.skinned.push_back(player_.buildItem(alpha));
    }

    // Удалённые игроки: сглаживаем к последней цели из снапшотов (интерполяция).
    float k = renderDt * 12.0f;
    if (k > 1.0f) k = 1.0f;
    for (RemotePlayer& r : remotes_) {
        r.ch.position = r.ch.position + (r.targetPos - r.ch.position) * k;
        r.ch.facingYaw = lerpAngle(r.ch.facingYaw, r.targetYaw, k);
        r.ch.animParam += (r.targetAnim - r.ch.animParam) * k;
        r.ch.animTime += renderDt;  // фаза анимации крутится локально
        frame.skinned.push_back(r.ch.buildItem(1.0f));  // alpha=1 -> текущее состояние
    }
    return frame;
}
