#include "Scene.h"

#include <cmath>

#include "Assets.h"
#include "Log.h"
#include "Renderer.h"

void Scene::build(Renderer& renderer, AssetSource& assets) {
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
        foxMesh_ = renderer.createSkinnedMesh(foxModel_);
        if (foxModel_.hasTexture) {
            foxTex_ = renderer.createTexture(foxModel_.baseColor);
        }
        player_.position = {0.0f, 0.0f, 0.0f};
        player_.snapshot();  // prev = curr, чтобы первый кадр не «прыгнул»
    } else {
        LOGW("Не удалось загрузить Fox.glb");
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

    tickDt_ = dt;

    // Отправляем ввод серверу и запоминаем его как неподтверждённый (для реплея).
    if (client_.connected()) {
        cmd.seq = ++inputSeq_;
        client_.sendInput(cmd);
        pending_.push_back({cmd, dt});
    }

    player_.snapshot();          // зафиксировать прошлое для интерполяции
    player_.simulate(dt, cmd);   // локальное предсказание

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

            size_t w = 0;  // выкидываем подтверждённые (seq <= ack)
            for (size_t i = 0; i < pending_.size(); ++i) {
                if (pending_[i].cmd.seq > ack) pending_[w++] = pending_[i];
            }
            pending_.resize(w);
            for (const PendingInput& p : pending_) {
                player_.simulate(p.dt, p.cmd);  // повторно применяем поверх сервера
            }
            continue;
        }

        // Чужой игрок: кладём снапшот в буфер интерполяции.
        RemotePlayer* r = nullptr;
        for (auto& rp : remotes_) {
            if (rp.id == s.id) { r = &rp; break; }
        }
        if (r == nullptr) {
            RemotePlayer rp;
            rp.id = s.id;
            rp.ch.position = {s.x, s.y, s.z};
            rp.ch.facingYaw = s.yaw;
            remotes_.push_back(rp);
            r = &remotes_.back();
        }
        r->buffer.push_back({simClock_, {s.x, s.y, s.z}, s.yaw, s.animParam});
        // Ограничиваем историю (~1 сек), чтобы буфер не рос.
        while (r->buffer.size() > 2 && r->buffer[1].t < simClock_ - 1.0) {
            r->buffer.erase(r->buffer.begin());
        }
    }

    // Убрать отключившихся.
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
    pending_.clear();
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
    for (RemotePlayer& r : remotes_) {
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
        frame.skinned.push_back(
            makeFoxItem(r.ch.position, r.ch.facingYaw, r.ch.animParam, r.ch.animTime));
    }
    return frame;
}
