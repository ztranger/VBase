// Выделенный (десктопный) сервер VBase. Использует ТОТ ЖЕ код симуляции и сети,
// что и клиент (Net.cpp / Character.cpp), но без Android/GL. Хост-режим в
// приложении больше не обязателен — можно поднять сервер отдельно.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>   // до windows.h, иначе конфликт с winsock v1
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "CollisionWorld.h"
#include "FileAssetSource.h"
#include "Net.h"
#include "SceneDesc.h"
#include "SceneLoader.h"

namespace {

// Headless-самотест кинематического контроллера (vbase_server --selftest).
// Гоним капсулу в стену (лобовое столкновение) и по касательной (скольжение),
// печатаем результат. Быстрый способ проверить collide-and-slide без GUI.
int runPhysicsSelfTest() {
    CollisionWorld world;
    world.addBox(Vec3{0.0f, -0.5f, 0.0f}, Vec3{50.0f, 0.5f, 50.0f});  // пол (верх на y=0)
    world.addBox(Vec3{2.0f, 1.0f, 0.0f}, Vec3{0.25f, 2.0f, 6.0f});    // стена на x=+2 (лицо 1.75)
    world.finalize();

    const float dt = 1.0f / 60.0f;
    const float r = 0.3f, cyl = 0.3f;

    // 1) Лобовое: скорость строго +X ~1.5 c. Должен упереться около x≈1.45 (1.75 - r).
    ColliderCharId a = world.addCharacter(Vec3{0.0f, 0.0f, 0.0f}, r, cyl);
    for (int i = 0; i < 90; ++i) world.moveCharacter(a, Vec3{5.0f, 0.0f, 0.0f}, false, dt);
    Vec3 pa = world.characterPosition(a);

    // 2) По диагонали в стену (+X,+Z): X должен упереться, Z — заметно вырасти (скольжение).
    ColliderCharId b = world.addCharacter(Vec3{0.0f, 0.0f, -2.0f}, r, cyl);
    for (int i = 0; i < 90; ++i) world.moveCharacter(b, Vec3{5.0f, 0.0f, 5.0f}, false, dt);
    Vec3 pb = world.characterPosition(b);

    // 3) Прыжок: сначала «прогрев» на земле (у свежего контроллера ground-state
    // становится OnGround лишь после первого Update), затем jump с земли.
    ColliderCharId c = world.addCharacter(Vec3{-4.0f, 0.0f, 0.0f}, r, cyl);
    for (int i = 0; i < 10; ++i) world.moveCharacter(c, Vec3{0.0f, 0.0f, 0.0f}, false, dt);
    world.moveCharacter(c, Vec3{0.0f, 0.0f, 0.0f}, true, dt);  // прыжок с земли
    float apex = 0.0f;
    for (int i = 0; i < 15; ++i) {  // ~0.25 c вверх — ловим верхнюю точку
        Vec3 p = world.moveCharacter(c, Vec3{0.0f, 0.0f, 0.0f}, false, dt);
        if (p.y > apex) apex = p.y;
    }
    for (int i = 0; i < 90; ++i) world.moveCharacter(c, Vec3{0.0f, 0.0f, 0.0f}, false, dt);  // падение
    Vec3 pc = world.characterPosition(c);

    std::printf("[SelfTest] лоб:    x=%.3f z=%.3f (ждём x<1.6, стоп у стены)\n",
                (double)pa.x, (double)pa.z);
    std::printf("[SelfTest] слайд:  x=%.3f z=%.3f (ждём x<1.6 и z заметно > -2)\n",
                (double)pb.x, (double)pb.z);
    std::printf("[SelfTest] прыжок: apex=%.3f y=%.3f (ждём apex>0.3, приземлился y≈0)\n",
                (double)apex, (double)pc.y);

    bool headOn = pa.x < 1.6f && pa.x > 1.2f;
    bool slide = pb.x < 1.6f && pb.z > 0.0f;         // упёрся по X, но проехал по Z
    bool jumped = apex > 0.3f && pc.y > -0.1f && pc.y < 0.1f;  // взлетел и вернулся на пол
    bool ok = headOn && slide && jumped;
    std::printf("[SelfTest] %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}

// Интеграционный тест серверного пути (сеть по loopback): клиент шлёт ввод в стену,
// авторитетный сервер симулирует с коллизиями и шлёт снапшоты — позиция должна
// упереться в стену. Проверяет связку connect -> спавн капсулы -> tick(world) -> снапшот.
int runServerPathTest() {
    SceneDesc desc;
    ColliderSpec floor;
    floor.center = Vec3{0.0f, -0.5f, 0.0f};
    floor.half = Vec3{50.0f, 0.5f, 50.0f};  // пол (верх на y=0)
    desc.colliders.push_back(floor);
    ColliderSpec wall;
    wall.center = Vec3{2.0f, 1.0f, 0.0f};
    wall.half = Vec3{0.25f, 2.0f, 6.0f};  // стена на x=+2 (лицо на 1.75)
    desc.colliders.push_back(wall);
    desc.player.pos = Vec3{0.0f, 0.0f, 0.0f};
    desc.player.colliderRadius = 0.3f;
    desc.player.colliderCylHalf = 0.3f;

    NetServer server;
    if (!server.start(kNetPort)) { std::printf("[ServerTest] FAIL: сервер не стартовал\n"); return 1; }
    server.configureWorld(desc);

    NetClient client;
    client.connect("127.0.0.1", kNetPort);

    const float dt = 1.0f / 60.0f;
    InputCommand cmd;
    cmd.moveX = 1.0f;   // строго в стену (+X)
    cmd.moveZ = 0.0f;
    cmd.magnitude = 1.0f;
    cmd.faceMove = true;

    float lastX = 0.0f;
    uint32_t seq = 0;
    for (int i = 0; i < 400; ++i) {
        server.poll();
        if (client.connected() && client.myId() != 0) {
            cmd.seq = ++seq;
            client.sendInput(cmd);
        }
        server.tick(dt);
        client.poll();
        if (client.consumeSnapshot()) {
            for (const EntityState& s : client.states())
                if (s.id == client.myId()) lastX = s.x;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));  // дать enet прокачать loopback
    }

    std::printf("[ServerTest] авторитетный x = %.3f (ждём <1.6, упор в стену; myId=%u)\n",
                (double)lastX, client.myId());
    bool ok = client.myId() != 0 && lastX > 1.0f && lastX < 1.6f;
    std::printf("[ServerTest] %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}

// Печатает локальные IPv4-адреса машины — чтобы не искать их вручную.
// Winsock уже инициализирован (enet_initialize в NetServer::start).
void printLocalAddresses(uint16_t port) {
    char host[256] = {0};
    if (gethostname(host, sizeof(host)) != 0) return;

    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    struct addrinfo* res = nullptr;
    if (getaddrinfo(host, nullptr, &hints, &res) != 0) return;

    std::printf("Адрес(а) для Join с телефона (та же Wi-Fi сеть):\n");
    for (struct addrinfo* p = res; p != nullptr; p = p->ai_next) {
        auto* a = reinterpret_cast<struct sockaddr_in*>(p->ai_addr);
        char ip[64] = {0};
        inet_ntop(AF_INET, &a->sin_addr, ip, sizeof(ip));
        if (std::strncmp(ip, "127.", 4) == 0) continue;  // пропускаем loopback
        std::printf("    %s:%u\n", ip, (unsigned)port);
    }
    freeaddrinfo(res);
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);  // консоль читает наш UTF-8, иначе кириллица — каша
#endif
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // логи сервера — сразу, без буфера

    if (argc > 1 && std::strcmp(argv[1], "--selftest") == 0) {
        int a = runPhysicsSelfTest();   // примитив: collide-and-slide
        int b = runServerPathTest();    // серверный путь: сеть -> авторитет с коллизиями
        return (a == 0 && b == 0) ? 0 : 1;
    }

    uint16_t port = kNetPort;
    if (argc > 1) {
        port = (uint16_t)std::atoi(argv[1]);
    }

    NetServer server;
    if (!server.start(port)) {
        std::fprintf(stderr, "Не удалось поднять сервер на порту %u\n", port);
        return 1;
    }
    std::printf("VBase server: слушаю UDP-порт %u (Ctrl+C для выхода)\n", port);
    printLocalAddresses(port);

    // Мир коллизий: грузим ТО ЖЕ описание сцены, что и клиент (та же геометрия —
    // иначе предсказание у стены разойдётся с авторитетом). Аргументы (как у десктопа):
    // argv[2] = каталог assets, argv[3] = путь сцены.
    std::string assetsDir = (argc > 2) ? argv[2] : "../../app/src/main/assets";
    const char* scenePath = (argc > 3) ? argv[3] : "scenes/default.scene";
    FileAssetSource assets(assetsDir);
    SceneDesc desc;
    if (loadSceneDesc(assets, scenePath, desc)) {
        server.configureWorld(desc);
    } else {
        std::fprintf(stderr, "Сцена не загружена (%s, assets=%s) — сервер без коллизий\n",
                     scenePath, assetsDir.c_str());
    }

    const double tick = 1.0 / 30.0;  // 30 Гц симуляции
    auto last = std::chrono::steady_clock::now();
    double accumulator = 0.0;
    int lastClients = -1;

    for (;;) {
        server.poll();  // принять подключения и ввод

        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - last).count();
        last = now;
        if (dt > 0.25) dt = 0.25;
        accumulator += dt;

        while (accumulator >= tick) {
            server.tick((float)tick);  // симуляция + рассылка снапшотов
            accumulator -= tick;
        }

        if (server.clientCount() != lastClients) {
            lastClients = server.clientCount();
            std::printf("Игроков онлайн: %d\n", lastClients);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
