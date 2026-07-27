// Десктопный клиент VBase (Windows). Переиспользует общее ядро: Character,
// FollowCamera, Net, InputCommand, генераторы мешей, MathUtil. Рендер — свой
// минимальный (DesktopRenderer), ввод — клавиатура (WASD). Подключается к серверу.
//
// MVP: игрок и другие игроки рисуются кубами (без лисы/скиннинга/текстур/HUD —
// им нужна абстракция ассетов, это следующий шаг). Позиции синхронизируются с
// сервером, так что виден реальный мультиплеер ПК <-> телефон.

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "Character.h"
#include "DesktopRenderer.h"
#include "FileAssetSource.h"
#include "FollowCamera.h"
#include "Input.h"
#include "Mesh.h"
#include "MathUtil.h"
#include "Model.h"
#include "Net.h"

namespace {

constexpr float kTick = 1.0f / 30.0f;

struct StaticObj {
    uint32_t mesh;
    Mat4 model;
    Vec3 color;
};

struct RemoteCube {
    uint32_t id = 0;
    Vec3 pos{0, 0, 0};
    float yaw = 0;
    Vec3 target{0, 0, 0};
    float targetYaw = 0;
};

int keyAxis(GLFWwindow* w, int pos, int neg) {
    int v = 0;
    if (glfwGetKey(w, pos) == GLFW_PRESS) v += 1;
    if (glfwGetKey(w, neg) == GLFW_PRESS) v -= 1;
    return v;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // логи сразу, без буфера
    const char* serverIp = (argc > 1) ? argv[1] : "127.0.0.1";

    if (!glfwInit()) {
        std::fprintf(stderr, "glfwInit failed\n");
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    GLFWwindow* window = glfwCreateWindow(1280, 720, "VBase Desktop", nullptr, nullptr);
    if (window == nullptr) {
        std::fprintf(stderr, "glfwCreateWindow failed\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);  // vsync

    DesktopRenderer renderer;
    // Грузим адреса GL-функций через GLFW (captureless-лямбда -> указатель функции).
    if (!renderer.init([](const char* n) { return (void*)glfwGetProcAddress(n); })) {
        return 1;
    }

    // Проверка абстракции ассетов на десктопе: читаем ту же Fox.glb с диска
    // тем же портируемым загрузчиком (рендер модели — следующий шаг).
    std::string assetsDir = (argc > 2) ? argv[2] : "../../app/src/main/assets";
    FileAssetSource assets(assetsDir);
    SkinnedModel fox;
    if (loadGltfModel(assets, "models/Fox.glb", fox)) {
        std::printf("Fox загружен с диска: %u вершин, %u анимаций, текстура %s\n",
                    (unsigned)fox.vertices.size(), (unsigned)fox.animations.size(),
                    fox.hasTexture ? "есть" : "нет");
    } else {
        std::printf("Fox не найден (assets dir: %s) — укажи путь 2-м аргументом\n",
                    assetsDir.c_str());
    }

    uint32_t planeMesh = renderer.createMesh(makePlane(24.0f, 1.0f));
    uint32_t cubeMesh = renderer.createMesh(makeCube(1.0f));
    uint32_t sphereMesh = renderer.createMesh(makeSphere(0.6f));

    // Статичное окружение.
    std::vector<StaticObj> statics;
    statics.push_back({planeMesh, Mat4::translation({0, 0, 0}), {0.30f, 0.32f, 0.38f}});
    for (int i = 0; i < 3; ++i) {
        float x = -4.0f + (float)i * 4.0f;
        Mat4 m = Mat4::translation({x, 0.5f, -4.0f});
        statics.push_back({cubeMesh, m, {0.9f, 0.4f, 0.3f}});
        Mat4 s = Mat4::translation({x, 0.6f, 4.0f});
        statics.push_back({sphereMesh, s, {0.35f, 0.55f, 0.95f}});
    }

    Character player;
    player.position = {0, 0, 0};
    player.snapshot();
    FollowCamera camera;

    NetClient client;
    client.connect(serverIp, kNetPort);
    std::printf("Подключение к серверу %s:%u...\n", serverIp, kNetPort);
    uint32_t inputSeq = 0;

    std::vector<RemoteCube> remotes;

    auto last = std::chrono::steady_clock::now();
    float accumulator = 0.0f;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;

        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        if (dt > 0.25f) dt = 0.25f;

        // Ввод: WASD -> вектор (как виртуальный стик).
        float jx = (float)keyAxis(window, GLFW_KEY_D, GLFW_KEY_A);
        float jy = (float)keyAxis(window, GLFW_KEY_W, GLFW_KEY_S);
        float mag = std::sqrt(jx * jx + jy * jy);
        if (mag > 1.0f) { jx /= mag; jy /= mag; mag = 1.0f; }

        accumulator += dt;
        while (accumulator >= kTick) {
            // Направление относительно камеры (как на телефоне).
            InputCommand cmd;
            if (mag > 0.05f) {
                float cy = camera.yaw;
                Vec3 fwd{std::sin(cy), 0.0f, std::cos(cy)};
                Vec3 right{-std::cos(cy), 0.0f, std::sin(cy)};
                Vec3 dir = normalize(fwd * jy + right * jx);
                cmd.moveX = dir.x;
                cmd.moveZ = dir.z;
            }
            cmd.faceMove = jy >= 0.0f;
            cmd.magnitude = (jy < 0.0f) ? mag * 0.5f : mag;

            if (client.connected()) {
                cmd.seq = ++inputSeq;
                client.sendInput(cmd);
            }
            player.snapshot();
            player.simulate(kTick, cmd);

            client.poll();
            if (client.consumeSnapshot()) {
                uint32_t myId = client.myId();
                const std::vector<EntityState>& states = client.states();
                for (const EntityState& s : states) {
                    if (myId != 0 && s.id == myId) {
                        // Мягкая коррекция своего аватара к серверу.
                        Vec3 sp{s.x, s.y, s.z};
                        player.position = player.position + (sp - player.position) * 0.15f;
                        player.facingYaw = lerpAngle(player.facingYaw, s.yaw, 0.15f);
                        continue;
                    }
                    RemoteCube* r = nullptr;
                    for (auto& rc : remotes) if (rc.id == s.id) { r = &rc; break; }
                    if (r == nullptr) {
                        RemoteCube rc;
                        rc.id = s.id;
                        rc.pos = {s.x, s.y, s.z};
                        rc.yaw = s.yaw;
                        remotes.push_back(rc);
                        r = &remotes.back();
                    }
                    r->target = {s.x, s.y, s.z};
                    r->targetYaw = s.yaw;
                }
                // Удалить ушедших.
                for (size_t i = 0; i < remotes.size();) {
                    bool found = false;
                    for (const EntityState& s : states) if (s.id == remotes[i].id) { found = true; break; }
                    if (!found) remotes.erase(remotes.begin() + (long)i); else ++i;
                }
            }
            accumulator -= kTick;
        }

        camera.follow(player.position, player.facingYaw, dt);

        // Сглаживание чужих к цели.
        float k = dt * 12.0f;
        if (k > 1.0f) k = 1.0f;
        for (RemoteCube& r : remotes) {
            r.pos = r.pos + (r.target - r.pos) * k;
            r.yaw = lerpAngle(r.yaw, r.targetYaw, k);
        }

        int fbw = 0, fbh = 0;
        glfwGetFramebufferSize(window, &fbw, &fbh);
        float aspect = fbh > 0 ? (float)fbw / (float)fbh : 1.0f;

        renderer.beginFrame(fbw, fbh, camera.view(), camera.proj(aspect),
                            normalize(Vec3{0.4f, 1.0f, 0.6f}));
        for (const StaticObj& o : statics) {
            renderer.draw(o.mesh, o.model, o.color);
        }
        // Свой игрок — жёлтый куб.
        Mat4 pm = Mat4::translation(player.position) * Mat4::rotationY(player.facingYaw)
                * Mat4::scale({0.6f, 0.6f, 0.6f});
        renderer.draw(cubeMesh, pm, {0.95f, 0.85f, 0.2f});
        // Чужие — зелёные кубы.
        for (const RemoteCube& r : remotes) {
            Mat4 rm = Mat4::translation(r.pos) * Mat4::rotationY(r.yaw)
                    * Mat4::scale({0.6f, 0.6f, 0.6f});
            renderer.draw(cubeMesh, rm, {0.3f, 0.9f, 0.4f});
        }

        glfwSwapBuffers(window);
    }

    client.disconnect();
    renderer.shutdown();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
