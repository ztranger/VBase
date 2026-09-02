// Выделенный (десктопный) сервер VBase. Использует ТОТ ЖЕ код симуляции и сети,
// что и клиент (Net.cpp / Character.cpp), но без Android/GL. Хост-режим в
// приложении больше не обязателен — можно поднять сервер отдельно.

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

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

#include "engine/physics/CollisionWorld.h"
#include "engine/assets/FileAssetSource.h"
#include "game/GameWorld.h"
#include "engine/net/Net.h"
#include "game/CharacterRoster.h"
#include "game/FlowField.h"
#include "game/SceneDesc.h"
#include "game/SceneLoader.h"

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
    float vyA = 0.0f;
    for (int i = 0; i < 90; ++i) world.moveCharacter(a, Vec3{5.0f, 0.0f, 0.0f}, vyA, false, dt);
    Vec3 pa = world.characterPosition(a);

    // 2) По диагонали в стену (+X,+Z): X должен упереться, Z — заметно вырасти (скольжение).
    ColliderCharId b = world.addCharacter(Vec3{0.0f, 0.0f, -2.0f}, r, cyl);
    float vyB = 0.0f;
    for (int i = 0; i < 90; ++i) world.moveCharacter(b, Vec3{5.0f, 0.0f, 5.0f}, vyB, false, dt);
    Vec3 pb = world.characterPosition(b);

    // 3) Прыжок: сначала «прогрев» на земле (у свежего контроллера ground-state
    // становится OnGround лишь после первого Update), затем jump с земли.
    ColliderCharId c = world.addCharacter(Vec3{-4.0f, 0.0f, 0.0f}, r, cyl);
    float vyC = 0.0f;
    for (int i = 0; i < 10; ++i) world.moveCharacter(c, Vec3{0.0f, 0.0f, 0.0f}, vyC, false, dt);
    world.moveCharacter(c, Vec3{0.0f, 0.0f, 0.0f}, vyC, true, dt);  // прыжок с земли
    float apex = 0.0f;
    for (int i = 0; i < 15; ++i) {  // ~0.25 c вверх — ловим верхнюю точку
        Vec3 p = world.moveCharacter(c, Vec3{0.0f, 0.0f, 0.0f}, vyC, false, dt);
        if (p.y > apex) apex = p.y;
    }
    for (int i = 0; i < 90; ++i) world.moveCharacter(c, Vec3{0.0f, 0.0f, 0.0f}, vyC, false, dt);  // падение
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
    // Фаза 1: гоним в стену. Фаза 2: простой (ввод mag=0) — состояние устаканивается,
    // дельта должна схлопнуться до нуля (сущность не пересылается).
    const int moveIters = 150, idleIters = 250;
    for (int i = 0; i < moveIters + idleIters; ++i) {
        if (i == moveIters) { cmd.moveX = 0.0f; cmd.magnitude = 0.0f; }  // -> простой
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
    int idleChanged = server.debugLastChanged();  // дельта на последнем (простойном) тике

    std::printf("[ServerTest] авторитетный x = %.3f (ждём <1.6, упор в стену; myId=%u)\n",
                (double)lastX, client.myId());
    std::printf("[ServerTest] дельта в простое: changed=%d (ждём 0 — сжатие работает)\n",
                idleChanged);
    bool posOk = client.myId() != 0 && lastX > 1.0f && lastX < 1.6f;  // дельта реконструирована верно
    bool deltaOk = idleChanged == 0;                                   // сжатие схлопнулось
    bool ok = posOk && deltaOk;
    std::printf("[ServerTest] %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}

// Тест экономики: генератор капает ресурс, хранилище ограничивает потолок.
int runEconomyTest() {
    SceneDesc desc;
    BuildingSpec gen;
    gen.kind = BuildingSpec::Generator;
    gen.pos = Vec3{0.0f, 0.0f, 0.0f};
    gen.rate = 10.0f;  // 10 ресурса/сек
    BuildingSpec sto;
    sto.kind = BuildingSpec::Storage;
    sto.pos = Vec3{2.0f, 0.0f, 0.0f};
    sto.cap = 25.0f;   // потолок 25
    desc.buildings.push_back(gen);
    desc.buildings.push_back(sto);

    NetServer server;
    if (!server.start(kNetPort)) { std::printf("[EconTest] FAIL: сервер не стартовал\n"); return 1; }
    server.configureWorld(desc);

    // Подключаем клиента — заодно проверим, что здание-хранилище со своим ресурсом
    // доезжает по сети и реконструируется (сервер -> снапшот -> клиент).
    NetClient client;
    client.connect("127.0.0.1", kNetPort);

    const float dt = 1.0f / 30.0f;
    InputCommand idle;  // герой стоит; экономика идёт независимо
    uint32_t seq = 0;
    int storageCount = 0;
    float clientAux = -1.0f;
    for (int i = 0; i < 150; ++i) {  // ~5 c
        server.poll();
        if (client.connected() && client.myId() != 0) { idle.seq = ++seq; client.sendInput(idle); }
        server.tick(dt);
        client.poll();
        if (client.consumeSnapshot()) {
            storageCount = 0;
            for (const EntityState& s : client.states())
                if ((EntityType)s.type == EntityType::Storage) { storageCount++; clientAux = s.aux; }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    float serverRes = server.debugResource();

    std::printf("[EconTest] сервер: пул=%.1f (ждём cap=25); клиент видит хранилищ=%d, ресурс=%.1f\n",
                (double)serverRes, storageCount, (double)clientAux);
    bool serverOk = serverRes > 24.0f && serverRes < 25.5f;               // упор в потолок
    bool clientOk = storageCount == 1 && clientAux > 24.0f && clientAux < 25.5f;  // доехало по сети
    bool ok = serverOk && clientOk;
    std::printf("[EconTest] %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}

// Тест спавнеров и врагов: спавнер по таймеру плодит врагов (до потолка), те бегут
// к ядру. Проверяем и число (потолок), и что добежали (по сети через клиента).
int runSpawnerTest() {
    SceneDesc desc;
    ColliderSpec floor;
    floor.center = Vec3{0.0f, -0.5f, 0.0f};
    floor.half = Vec3{50.0f, 0.5f, 50.0f};
    desc.colliders.push_back(floor);
    BuildingSpec core;
    core.kind = BuildingSpec::Core;
    core.pos = Vec3{0.0f, 0.0f, 0.0f};
    desc.buildings.push_back(core);
    BuildingSpec sp;
    sp.kind = BuildingSpec::Spawner;
    sp.pos = Vec3{8.0f, 0.0f, 0.0f};
    sp.rate = 0.5f;  // интервал 0.5 c
    sp.cap = 3.0f;   // максимум 3 врага
    desc.buildings.push_back(sp);

    NetServer server;
    if (!server.start(kNetPort)) { std::printf("[SpawnTest] FAIL: сервер не стартовал\n"); return 1; }
    server.configureWorld(desc);

    NetClient client;
    client.connect("127.0.0.1", kNetPort);

    const float dt = 1.0f / 30.0f;
    InputCommand idle;
    uint32_t seq = 0;
    int clientEnemies = 0;
    float nearest = 999.0f;
    for (int i = 0; i < 150; ++i) {  // ~5 c
        server.poll();
        if (client.connected() && client.myId() != 0) { idle.seq = ++seq; client.sendInput(idle); }
        server.tick(dt);
        client.poll();
        if (client.consumeSnapshot()) {
            clientEnemies = 0;
            nearest = 999.0f;
            for (const EntityState& s : client.states())
                if ((EntityType)s.type == EntityType::Enemy) {
                    clientEnemies++;
                    float d = std::sqrt(s.x * s.x + s.z * s.z);  // расстояние до ядра (0,0,0)
                    if (d < nearest) nearest = d;
                }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    int serverEnemies = server.debugEnemyCount();

    std::printf("[SpawnTest] сервер врагов=%d (ждём 3); клиент видит=%d, ближайший до ядра=%.1f "
                "(ждём ~1, добежал от 8)\n",
                serverEnemies, clientEnemies, (double)nearest);
    bool spawnOk = serverEnemies == 3 && clientEnemies == 3;  // потолок + стриминг
    bool moveOk = nearest < 3.0f;                             // добежали к ядру (футпринт держит на ~2.6)
    bool ok = spawnOk && moveOk;
    std::printf("[SpawnTest] %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}

// Тест боя + жизненного цикла матча. Два сценария на одном коде:
//  - без башен: враги добегают до ядра и разбивают его -> ПОРАЖЕНИЕ (phase=2), hp<=0;
//  - с башнями: башни выбивают всех врагов -> ПОБЕДА (phase=1), врагов 0.
// Фазу читаем С ПРОВОДА (client.gamePhase()) — заодно проверяем новый байт в заголовке
// снапшота и версию протокола v2.
int runCombatTest() {
    auto scenario = [](bool withTowers, int& outPhase, float& outCoreHp, int& outEnemies) {
        SceneDesc desc;
        ColliderSpec floor;
        floor.center = Vec3{0.0f, -0.5f, 0.0f};
        floor.half = Vec3{50.0f, 0.5f, 50.0f};
        desc.colliders.push_back(floor);

        BuildingSpec core;
        core.kind = BuildingSpec::Core;
        core.pos = Vec3{0.0f, 0.0f, 0.0f};
        core.hp = withTowers ? 500.0f : 20.0f;  // без защиты ядро хрупкое
        desc.buildings.push_back(core);

        BuildingSpec sp;
        sp.kind = BuildingSpec::Spawner;
        sp.pos = Vec3{8.0f, 0.0f, 0.0f};
        sp.rate = 0.4f;                    // враг каждые 0.4 c
        sp.cap = withTowers ? 3.0f : 5.0f;
        desc.buildings.push_back(sp);

        if (withTowers) {
            BuildingSpec tw;
            tw.kind = BuildingSpec::Tower;
            tw.pos = Vec3{3.0f, 0.0f, 0.0f};
            tw.damage = 25.0f;             // > hp врага -> с одного выстрела
            tw.range = 7.0f;
            tw.rate = 0.2f;
            desc.buildings.push_back(tw);
        }
        desc.enemy.hp = 20.0f;
        desc.enemy.damage = 10.0f;
        desc.enemy.attackInterval = 0.5f;
        desc.player.pos = Vec3{0.0f, 0.0f, 12.0f};  // герой клиента — в стороне от боя

        NetServer server;
        server.start(kNetPort);
        server.configureWorld(desc);
        NetClient client;
        client.connect("127.0.0.1", kNetPort);

        const float dt = kTickDt;
        InputCommand idle;
        uint32_t seq = 0;
        for (int i = 0; i < 700; ++i) {  // ~23 c симуляции — с запасом на исход
            server.poll();
            if (client.connected() && client.myId() != 0) { idle.seq = ++seq; client.sendInput(idle); }
            server.tick(dt);
            client.poll();
            client.consumeSnapshot();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        outPhase = (int)client.gamePhase();  // с провода
        outCoreHp = server.debugCoreHp();
        outEnemies = server.debugEnemyCount();
    };

    int lostPhase = 0, wonPhase = 0, lostEnemies = 0, wonEnemies = 0;
    float lostHp = 0.0f, wonHp = 0.0f;
    scenario(false, lostPhase, lostHp, lostEnemies);
    scenario(true, wonPhase, wonHp, wonEnemies);

    std::printf("[CombatTest] без защиты: phase=%d (ждём 2=поражение), hp ядра=%.1f (ждём <=0)\n",
                lostPhase, (double)lostHp);
    std::printf("[CombatTest] с башнями:  phase=%d (ждём 1=победа), врагов=%d (ждём 0)\n",
                wonPhase, wonEnemies);
    bool lostOk = lostPhase == 2 && lostHp <= 0.0f;
    bool wonOk = wonPhase == 1 && wonEnemies == 0;
    bool ok = lostOk && wonOk;
    std::printf("[CombatTest] %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}

// Тест размещения зданий героем (G3-B): клиент шлёт MSG_BUILD, сервер валидирует на сетке.
// Валидная клетка -> башня появляется в снапшоте; клетка здания и вне-арены -> отказ.
int runBuildTest() {
    SceneDesc desc;
    ColliderSpec floor;
    floor.center = Vec3{0.0f, -0.5f, 0.0f};
    floor.half = Vec3{50.0f, 0.5f, 50.0f};
    desc.colliders.push_back(floor);
    BuildingSpec gen;  // генератор быстро копит ресурс (клетка (0,0) — цель теста «занято»)
    gen.kind = BuildingSpec::Generator;
    gen.pos = Vec3{0.0f, 0.0f, 0.0f};
    gen.rate = 50.0f;
    desc.buildings.push_back(gen);
    BuildingSpec sto;
    sto.kind = BuildingSpec::Storage;
    sto.pos = Vec3{4.0f, 0.0f, 0.0f};
    sto.cap = 300.0f;
    desc.buildings.push_back(sto);
    // Шаблон башни задаём вручную (тест не грузит конфиг): {buildable,cost,rate,cap,hp,damage,range}.
    desc.build[(int)EntityType::Tower] = BuildTemplate{true, 50.0f, 0.5f, 0.0f, 0.0f, 8.0f, 5.0f};
    desc.player.pos = Vec3{0.0f, 0.0f, 10.0f};

    NetServer server;
    if (!server.start(kNetPort)) { std::printf("[BuildTest] FAIL: сервер не стартовал\n"); return 1; }
    server.configureWorld(desc);
    NetClient client;
    client.connect("127.0.0.1", kNetPort);

    const float dt = kTickDt;
    InputCommand idle;
    uint32_t seq = 0;
    bool sentValid = false, sentBad = false;
    int towers = 0;
    for (int i = 0; i < 400; ++i) {
        server.poll();
        if (client.connected() && client.myId() != 0) { idle.seq = ++seq; client.sendInput(idle); }
        if (!sentValid && server.debugResource() >= 100.0f) {
            client.sendBuild((uint8_t)EntityType::Tower, 3, 3);  // свободная клетка -> ОК
            sentValid = true;
        } else if (sentValid && !sentBad) {
            client.sendBuild((uint8_t)EntityType::Tower, 0, 0);        // клетка генератора -> занято
            client.sendBuild((uint8_t)EntityType::Tower, 100, 100);   // вне арены -> отказ
            sentBad = true;
        }
        server.tick(dt);
        client.poll();
        if (client.consumeSnapshot()) {
            towers = 0;
            for (const EntityState& s : client.states())
                if ((EntityType)s.type == EntityType::Tower) towers++;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    std::printf("[BuildTest] башен по проводу=%d (ждём 1: валид принят, занято/вне-арены отвергнуты)\n",
                towers);
    bool ok = towers == 1;
    std::printf("[BuildTest] %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}

// Тест ресурса per-team (#4): у каждой команды свой пул. Гоняем GameWorld напрямую (без
// сети): пулы двух команд наполняются независимо в свои потолки, а tryBuild списывает пул
// строителя, не трогая чужой.
int runTeamEconomyTest() {
    GameWorld world;
    SceneDesc desc;
    BuildingSpec g0; g0.kind = BuildingSpec::Generator; g0.pos = Vec3{0,0,0}; g0.rate = 10.0f; g0.team = 0;
    BuildingSpec s0; s0.kind = BuildingSpec::Storage;   s0.pos = Vec3{2,0,0}; s0.cap = 50.0f;  s0.team = 0;
    BuildingSpec g1; g1.kind = BuildingSpec::Generator; g1.pos = Vec3{0,0,4}; g1.rate = 20.0f; g1.team = 1;
    BuildingSpec s1; s1.kind = BuildingSpec::Storage;   s1.pos = Vec3{2,0,4}; s1.cap = 100.0f; s1.team = 1;
    desc.buildings = {g0, s0, g1, s1};
    desc.build[(int)EntityType::Tower] = BuildTemplate{true, 30.0f, 0.5f, 0.0f, 0.0f, 8.0f, 5.0f};
    world.configure(desc);

    const float dt = kTickDt;
    for (int i = 0; i < 300; ++i) world.step(dt);  // ~10 c — пулы упираются в потолки
    float r0 = world.resource(0), r1 = world.resource(1);
    bool econOk = r0 > 49.0f && r0 < 51.0f && r1 > 99.0f && r1 < 101.0f;  // независимо, в свой cap

    // Строитель команды 1 ставит башню — списывается пул team1, team0 не меняется.
    uint32_t hero1 = world.addHero(1);
    float b0 = world.resource(0), b1 = world.resource(1);
    bool built = world.tryBuild(hero1, EntityType::Tower, 3, 3);
    float a0 = world.resource(0), a1 = world.resource(1);
    bool buildOk = built && a0 == b0 && (b1 - a1) > 29.0f && (b1 - a1) < 31.0f;

    std::printf("[TeamEcon] пулы: team0=%.0f (ждём 50), team1=%.0f (ждём 100)\n", (double)r0, (double)r1);
    std::printf("[TeamEcon] постройка team1: списано team1=%.0f (ждём 30), team0 не тронут=%s\n",
                (double)(b1 - a1), (a0 == b0) ? "да" : "нет");
    bool ok = econOk && buildOk;
    std::printf("[TeamEcon] %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}

// Тест ставок героя: герой стоит на пути потока врагов, получает урон, повержен (hp<=0),
// затем возрождается в точке спавна с полным hp. Гоняем GameWorld напрямую.
int runHeroStakesTest() {
    GameWorld world;
    SceneDesc desc;
    ColliderSpec floor; floor.center = Vec3{0,-0.5f,0}; floor.half = Vec3{50,0.5f,50}; desc.colliders.push_back(floor);
    BuildingSpec core; core.kind = BuildingSpec::Core; core.pos = Vec3{0,0,0}; core.hp = 100000.0f; desc.buildings.push_back(core);
    BuildingSpec sp; sp.kind = BuildingSpec::Spawner; sp.pos = Vec3{6,0,0}; sp.rate = 0.3f; sp.cap = 20.0f; desc.buildings.push_back(sp);
    desc.enemy.hp = 100000.0f;   // враги не умирают (нет защиты) — стабильный поток мимо героя
    desc.enemy.damage = 20.0f;
    desc.enemy.attackInterval = 0.25f;
    desc.player.pos = Vec3{3,0,0};  // прямо на пути врагов (spawner 6 -> core 0)
    desc.player.hp = 40.0f;
    desc.player.respawnDelay = 1.0f;
    world.configure(desc);
    uint32_t hero = world.addHero(0);

    const float dt = kTickDt;
    float minHp = 999.0f;
    bool died = false, respawned = false;
    for (int i = 0; i < 700; ++i) {  // ~23 c: волна проходит, герой гибнет и возрождается
        world.step(dt);
        float hp = world.heroHp(hero);
        if (hp < minHp) minHp = hp;
        if (hp <= 0.0f) died = true;
        if (died && hp >= 39.0f) respawned = true;  // вернулся к полному hp
    }
    Vec3 pos = world.heroPos(hero);
    bool posOk = std::fabs(pos.x - 3.0f) < 0.7f && std::fabs(pos.z) < 0.7f;  // респаун у точки спавна

    std::printf("[HeroStakes] min hp героя=%.0f (ждём <=0 — повержен), возродился=%s, у спавна=%s\n",
                (double)minHp, respawned ? "да" : "нет", posOk ? "да" : "нет");
    bool ok = died && respawned && posOk;
    std::printf("[HeroStakes] %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}

// Авто-атака героя (headless): герой бьёт ближайшего врага в range при линии видимости.
// A: melee-герой на подступе к ядру выкашивает врагов -> их мало. B: стена по линии
// видимости между героем и ядром -> герой не бьёт -> враги копятся (LOS работает).
// C: маг (ranged) стреляет снарядами издалека -> враги гибнут (путь снаряда + хоминг).
int runHeroAttackTest() {
    // Общая арена: враги спавнятся на (9,0,0), идут к ядру (0,0,0) по полосе z=0.
    auto makeDesc = [](bool wall, bool ranged, Vec3 heroPos) {
        SceneDesc d;
        ColliderSpec floor; floor.center = Vec3{0, -0.5f, 0}; floor.half = Vec3{50, 0.5f, 50};
        d.colliders.push_back(floor);
        BuildingSpec core; core.kind = BuildingSpec::Core; core.pos = Vec3{0, 0, 0};
        core.hp = 100000.0f; d.buildings.push_back(core);        // ядро не падает — стабильный поток
        BuildingSpec sp; sp.kind = BuildingSpec::Spawner; sp.pos = Vec3{9, 0, 0};
        sp.rate = 0.5f; sp.cap = 40.0f; d.buildings.push_back(sp);
        d.enemy.hp = 15.0f; d.enemy.damage = 1.0f; d.enemy.attackInterval = 1.0f;
        if (wall) {  // СПЛОШНАЯ стена поперёк (z=4) между дальнобойным героем (z=8) и полосой врагов
            ColliderSpec w; w.center = Vec3{0, 1, 4}; w.half = Vec3{12, 1, 0.5f};  // не касается z=0 (путь свободен)
            d.colliders.push_back(w);
        }
        CharacterDesc hc; hc.id = "hero"; hc.model = "x"; hc.hp = 10000.0f;  // герой не гибнет
        hc.damage = 100.0f; hc.attackInterval = ranged ? 0.4f : 0.3f;
        hc.range = ranged ? 14.0f : 7.0f; hc.ranged = ranged;
        d.heroTypes.push_back(hc);
        d.player.pos = heroPos;
        return d;
    };
    auto runN = [](const SceneDesc& d) {
        GameWorld w; w.configure(d);
        uint32_t h = w.addHero(0); w.setHeroCharType(h, 0);
        for (int i = 0; i < 400; ++i) w.step(kTickDt);  // ~13 c
        return w.enemyCount();
    };

    // A: melee-герой у полосы (z=3, range 7) выкашивает проходящих врагов -> их мало.
    int countA = runN(makeDesc(false, false, Vec3{0, 0, 3}));
    // C: маг (ranged, z=8, range 14) стреляет снарядами -> враги гибнут (путь снаряда + хоминг).
    int countC = runN(makeDesc(false, true, Vec3{0, 0, 8}));
    // B: тот же маг, но СПЛОШНАЯ стена по линии видимости -> не бьёт -> враги копятся (LOS работает).
    int countB = runN(makeDesc(true, true, Vec3{0, 0, 8}));

    // D: PvP — герой team1 и герой team2 рядом бьют ДРУГ ДРУГА (не только мобов).
    SceneDesc dd;
    ColliderSpec ff; ff.center = Vec3{0, -0.5f, 0}; ff.half = Vec3{50, 0.5f, 50}; dd.colliders.push_back(ff);
    SpawnSpec sp1; sp1.team = 1; sp1.pos = Vec3{0, 0, 0}; dd.spawns.push_back(sp1);
    SpawnSpec sp2; sp2.team = 2; sp2.pos = Vec3{2, 0, 0}; dd.spawns.push_back(sp2);  // 2 < range 3
    CharacterDesc hd; hd.id = "h"; hd.model = "x"; hd.hp = 100.0f; hd.damage = 20.0f;
    hd.attackInterval = 0.4f; hd.range = 3.0f; hd.ranged = false;
    dd.heroTypes.push_back(hd);
    GameWorld wd; wd.configure(dd);
    uint32_t h1 = wd.addHero(1); wd.setHeroCharType(h1, 0);
    uint32_t h2 = wd.addHero(2); wd.setHeroCharType(h2, 0);
    (void)h2;
    float minHp1 = 999.0f;
    for (int i = 0; i < 200; ++i) { wd.step(kTickDt); float hp = wd.heroHp(h1); if (hp < minHp1) minHp1 = hp; }

    std::printf("[HeroAttack] melee=%d, маг=%d, маг+стена(LOS)=%d; PvP герой1 min hp=%.0f (ждём <100)\n",
                countA, countC, countB, (double)minHp1);
    bool ok = countA <= 4 && countC <= 6 && countB > countC && countB >= 8 && minHp1 < 100.0f;
    std::printf("[HeroAttack] %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}

// Тест team-aware таргетинга (фундамент PvP): бой считает целью только ВРАЖДЕБНЫЕ команды.
// Ч.1 «свои не бьют своих» (всё team 1): враги НЕ валят своё ядро, башня НЕ бьёт своих врагов.
// Ч.2 «чужих бьют» (враги team 1 vs ядро team 2): чужое ядро получает урон. Гоняем GameWorld
// напрямую (headless). PvE (всё team 0) не затронут — там hostile(0,0)=true, см. CombatTest.
int runTeamCombatTest() {
    ColliderSpec floor;
    floor.center = Vec3{0, -0.5f, 0};
    floor.half = Vec3{50, 0.5f, 50};

    // Ч.1: всё team 1 — дружественный огонь выключен.
    float friendlyCoreHp = 0.0f;
    int friendlyEnemies = 0;
    {
        GameWorld world;
        SceneDesc desc;
        desc.colliders.push_back(floor);
        BuildingSpec core; core.kind = BuildingSpec::Core; core.pos = Vec3{0,0,0}; core.hp = 100.0f; core.team = 1; desc.buildings.push_back(core);
        BuildingSpec sp; sp.kind = BuildingSpec::Spawner; sp.pos = Vec3{5,0,0}; sp.rate = 0.3f; sp.cap = 5.0f; sp.team = 1; desc.buildings.push_back(sp);
        BuildingSpec tw; tw.kind = BuildingSpec::Tower; tw.pos = Vec3{2,0,0}; tw.damage = 100.0f; tw.range = 8.0f; tw.rate = 0.3f; tw.team = 1; desc.buildings.push_back(tw);
        desc.enemy.hp = 50.0f; desc.enemy.damage = 20.0f; desc.enemy.attackInterval = 0.3f;
        world.configure(desc);
        for (int i = 0; i < 250; ++i) world.step(kTickDt);  // ~8 c
        friendlyCoreHp = world.coreHp();       // ядро team1 (единственное) — не должно пострадать
        friendlyEnemies = world.enemyCount();  // враги должны выжить (башня своих не бьёт)
    }

    // Ч.2: враги team 1 vs ядро team 2 — чужое ядро получает урон.
    float enemyCoreHp = 0.0f;
    {
        GameWorld world;
        SceneDesc desc;
        desc.colliders.push_back(floor);
        BuildingSpec core; core.kind = BuildingSpec::Core; core.pos = Vec3{0,0,0}; core.hp = 100.0f; core.team = 2; desc.buildings.push_back(core);
        BuildingSpec sp; sp.kind = BuildingSpec::Spawner; sp.pos = Vec3{6,0,0}; sp.rate = 0.3f; sp.cap = 5.0f; sp.team = 1; desc.buildings.push_back(sp);
        desc.enemy.hp = 50.0f; desc.enemy.damage = 20.0f; desc.enemy.attackInterval = 0.3f;
        world.configure(desc);
        for (int i = 0; i < 350; ++i) world.step(kTickDt);  // ~12 c — добежать и бить
        enemyCoreHp = world.coreHp();  // ядро team2 — должно быть повреждено
    }

    std::printf("[TeamCombat] свои: ядро team1 hp=%.0f (ждём 100), врагов=%d (ждём >0 — башня своих не бьёт)\n",
                (double)friendlyCoreHp, friendlyEnemies);
    std::printf("[TeamCombat] чужие: ядро team2 hp=%.0f (ждём <100 — враги team1 его бьют)\n",
                (double)enemyCoreHp);
    bool friendlyOk = friendlyCoreHp >= 99.0f && friendlyEnemies > 0;
    bool hostileOk = enemyCoreHp < 100.0f;
    bool ok = friendlyOk && hostileOk;
    std::printf("[TeamCombat] %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}

// Тест PvP (G5): назначение стороны + спавн-точки + per-team исход. Две базы (team 1/2) со
// своими spawn-точками; подключаем двух игроков — балансируются 1 и 2, каждый у своей точки.
// Спавнер team1 шлёт врагов на ядро team2 → team2 проигрывает, team1 побеждает. GameWorld напрямую.
int runPvpTest() {
    GameWorld world;
    SceneDesc desc;
    ColliderSpec floor; floor.center = Vec3{0, -0.5f, 0}; floor.half = Vec3{50, 0.5f, 50}; desc.colliders.push_back(floor);
    SpawnSpec sp1; sp1.team = 1; sp1.pos = Vec3{-12, 0, 0}; desc.spawns.push_back(sp1);
    SpawnSpec sp2; sp2.team = 2; sp2.pos = Vec3{12, 0, 0};  desc.spawns.push_back(sp2);
    BuildingSpec c1; c1.kind = BuildingSpec::Core; c1.pos = Vec3{-10, 0, 0}; c1.hp = 1000.0f; c1.team = 1; desc.buildings.push_back(c1);
    BuildingSpec c2; c2.kind = BuildingSpec::Core; c2.pos = Vec3{10, 0, 0};  c2.hp = 100.0f;  c2.team = 2; desc.buildings.push_back(c2);
    BuildingSpec s1; s1.kind = BuildingSpec::Spawner; s1.pos = Vec3{6, 0, 0}; s1.rate = 0.3f; s1.cap = 5.0f; s1.team = 1; desc.buildings.push_back(s1);
    desc.enemy.hp = 50.0f; desc.enemy.damage = 20.0f; desc.enemy.attackInterval = 0.3f;
    world.configure(desc);

    // Назначение сторон: два подключившихся игрока -> балансировка 1 и 2.
    uint32_t p1 = world.addPlayer();
    uint32_t p2 = world.addPlayer();
    uint8_t t1 = world.teamOf(p1), t2 = world.teamOf(p2);
    Vec3 pos1 = world.heroPos(p1), pos2 = world.heroPos(p2);  // сразу после спавна (у своих точек)

    for (int i = 0; i < 400; ++i) world.step(kTickDt);  // ~13 c — враги team1 валят ядро team2

    GamePhase ph1 = world.phaseForTeam(1), ph2 = world.phaseForTeam(2);

    bool teamsOk = (t1 == 1 && t2 == 2) || (t1 == 2 && t2 == 1);  // игроки на разных сторонах
    float want1 = (t1 == 1) ? -12.0f : 12.0f;                     // спавн у точки своей стороны
    float want2 = (t2 == 1) ? -12.0f : 12.0f;
    bool spawnOk = std::fabs(pos1.x - want1) < 0.5f && std::fabs(pos2.x - want2) < 0.5f;
    bool outcomeOk = ph2 == GamePhase::Lost && ph1 == GamePhase::Won;

    std::printf("[PvP] стороны игроков: %d,%d (ждём {1,2}); спавн у своих точек=%s\n",
                (int)t1, (int)t2, spawnOk ? "да" : "нет");
    std::printf("[PvP] исход: team1=%d team2=%d (ждём 1=победа/2=поражение)\n", (int)ph1, (int)ph2);
    bool ok = teamsOk && spawnOk && outcomeOk;
    std::printf("[PvP] %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}

// Тест матч-рестарта: после исхода мир заморожен, по таймеру (kMatchRestartDelay) авто-
// пересобирается (базы/герои/экономика), исход сбрасывается в Playing — и матч отыгрывается
// снова. Проверяем цикл decided → рестарт → decided. GameWorld напрямую.
int runMatchRestartTest() {
    GameWorld world;
    SceneDesc desc;
    ColliderSpec floor; floor.center = Vec3{0, -0.5f, 0}; floor.half = Vec3{50, 0.5f, 50}; desc.colliders.push_back(floor);
    SpawnSpec sp1; sp1.team = 1; sp1.pos = Vec3{-12, 0, 0}; desc.spawns.push_back(sp1);
    SpawnSpec sp2; sp2.team = 2; sp2.pos = Vec3{12, 0, 0};  desc.spawns.push_back(sp2);
    BuildingSpec c1; c1.kind = BuildingSpec::Core; c1.pos = Vec3{-10, 0, 0}; c1.hp = 100000.0f; c1.team = 1; desc.buildings.push_back(c1);  // team1 неубиваемо
    BuildingSpec c2; c2.kind = BuildingSpec::Core; c2.pos = Vec3{10, 0, 0};  c2.hp = 100.0f;    c2.team = 2; desc.buildings.push_back(c2);
    BuildingSpec s1; s1.kind = BuildingSpec::Spawner; s1.pos = Vec3{6, 0, 0}; s1.rate = 0.3f; s1.cap = 100.0f; s1.team = 1; desc.buildings.push_back(s1);
    desc.enemy.hp = 50.0f; desc.enemy.damage = 20.0f; desc.enemy.attackInterval = 0.3f;
    desc.matchRestartDelay = 8.0f;  // включаем авто-рестарт (в PvE-тестах он выкл по умолчанию)
    world.configure(desc);
    world.addPlayer(); world.addPlayer();

    bool sawDecision = false, phase2LostAtDecision = false, sawRestart = false, sawSecondDecision = false;
    for (int i = 0; i < 1200; ++i) {  // ~40 c: матч1 → исход → рестарт (8 c) → матч2 → исход
        world.step(kTickDt);
        if (world.decided()) {
            if (!sawDecision) { sawDecision = true; phase2LostAtDecision = (world.phaseForTeam(2) == GamePhase::Lost); }
            else if (sawRestart) sawSecondDecision = true;  // снова decided ПОСЛЕ рестарта = отыгран матч2
        } else if (sawDecision) {
            sawRestart = true;  // после первого исхода decided вернулось в false = рестарт сработал
        }
    }
    std::printf("[MatchRestart] исход1 (team2 Lost)=%s, рестарт (decided->false)=%s, исход2=%s\n",
                phase2LostAtDecision ? "да" : "нет", sawRestart ? "да" : "нет", sawSecondDecision ? "да" : "нет");
    bool ok = phase2LostAtDecision && sawRestart && sawSecondDecision;
    std::printf("[MatchRestart] %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}

// Кооп-тест (G4): два клиента на одной команде (team 0) делят базу. Оба видят друг друга
// (по 2 героя в снапшоте), оба строят из ОБЩЕГО пула на разных клетках; после дисконнекта
// одного база и второй герой остаются.
int runCoopTest() {
    SceneDesc desc;
    ColliderSpec floor;
    floor.center = Vec3{0, -0.5f, 0};
    floor.half = Vec3{50, 0.5f, 50};
    desc.colliders.push_back(floor);
    BuildingSpec gen; gen.kind = BuildingSpec::Generator; gen.pos = Vec3{0,0,0};  gen.rate = 50.0f; desc.buildings.push_back(gen);
    BuildingSpec sto; sto.kind = BuildingSpec::Storage;   sto.pos = Vec3{-2,0,0}; sto.cap = 300.0f; desc.buildings.push_back(sto);
    desc.build[(int)EntityType::Tower] = BuildTemplate{true, 30.0f, 0.5f, 0.0f, 0.0f, 8.0f, 5.0f};
    desc.player.pos = Vec3{0, 0, 8};

    NetServer server;
    if (!server.start(kNetPort)) { std::printf("[CoopTest] FAIL: сервер не стартовал\n"); return 1; }
    server.configureWorld(desc);
    NetClient a, b;
    a.connect("127.0.0.1", kNetPort);
    b.connect("127.0.0.1", kNetPort);

    const float dt = kTickDt;
    InputCommand idle;
    uint32_t sa = 0, sb = 0;
    bool builtA = false, builtB = false;
    int aHeroes = 0, bHeroes = 0, towers = 0;
    for (int i = 0; i < 500; ++i) {
        server.poll();
        if (a.connected() && a.myId() != 0) { idle.seq = ++sa; a.sendInput(idle); }
        if (b.connected() && b.myId() != 0) { idle.seq = ++sb; b.sendInput(idle); }
        if (server.debugResource() >= 100.0f) {  // накопился общий пул -> оба строят
            if (!builtA && a.myId() != 0) { a.sendBuild((uint8_t)EntityType::Tower, 3, 3); builtA = true; }
            if (!builtB && b.myId() != 0) { b.sendBuild((uint8_t)EntityType::Tower, -3, 3); builtB = true; }
        }
        server.tick(dt);
        a.poll(); b.poll();
        if (a.consumeSnapshot()) {
            aHeroes = 0; towers = 0;
            for (const EntityState& s : a.states()) {
                if ((EntityType)s.type == EntityType::Hero) aHeroes++;
                if ((EntityType)s.type == EntityType::Tower) towers++;
            }
        }
        if (b.consumeSnapshot()) {
            bHeroes = 0;
            for (const EntityState& s : b.states())
                if ((EntityType)s.type == EntityType::Hero) bHeroes++;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    bool coopOk = aHeroes == 2 && bHeroes == 2 && towers == 2;  // видят друг друга + оба построили

    // Фаза 2: b уходит — база (башни) и герой a остаются.
    b.disconnect();
    int aHeroesAfter = 0, towersAfter = 0;
    for (int i = 0; i < 100; ++i) {
        server.poll();
        if (a.myId() != 0) { idle.seq = ++sa; a.sendInput(idle); }
        server.tick(dt);
        a.poll();
        if (a.consumeSnapshot()) {
            aHeroesAfter = 0; towersAfter = 0;
            for (const EntityState& s : a.states()) {
                if ((EntityType)s.type == EntityType::Hero) aHeroesAfter++;
                if ((EntityType)s.type == EntityType::Tower) towersAfter++;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    bool dcOk = aHeroesAfter == 1 && towersAfter == 2;  // остался только свой герой, база цела

    std::printf("[CoopTest] у обоих клиентов героев=%d/%d (ждём 2/2), башен=%d (ждём 2)\n",
                aHeroes, bHeroes, towers);
    std::printf("[CoopTest] после ухода b: героев у a=%d (ждём 1), башен=%d (ждём 2)\n",
                aHeroesAfter, towersAfter);
    bool ok = coopOk && dcOk;
    std::printf("[CoopTest] %s\n", ok ? "OK" : "FAIL");
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

// docker stop шлёт SIGTERM; Ctrl+C — SIGINT. Без обработчика контейнер ждёт timeout и SIGKILL.
std::atomic<bool> gRunning{true};

void onStopSignal(int) {
    gRunning = false;
}

bool runningInDocker() {
#ifndef _WIN32
    if (std::FILE* f = std::fopen("/.dockerenv", "r")) {
        std::fclose(f);
        return true;
    }
#endif
    return false;
}

// Пасфайндинг мобов: поле потока на сетке произвольного размера, цели по типу,
// фолбэк «нет пути к ядру → ломать ближайшее здание».
int runPathfindTest() {
    {
        Grid g;
        g.cell = 2.0f;
        g.arenaHalf = 400.0f;
        NavGrid nav;
        nav.reset(g);
        const int n = nav.cellCount();
        FlowField f;
        f.compute(nav, {NavCell{0, 0}});
        const bool scaleOk =
            n >= 150000 && n <= 170000 && f.reachable(80, 80) && !f.reachable(10000, 0);
        std::printf("[PathTest] сетка ~400: клеток=%d reachable(80,80)=%d\n", n,
                    (int)f.reachable(80, 80));
        if (!scaleOk) {
            std::printf("[PathTest] FAIL: масштаб сетки\n");
            return 1;
        }
    }

    auto makeArena = [](SceneDesc& desc) {
        ColliderSpec floor;
        floor.center = Vec3{0.0f, -0.5f, 0.0f};
        floor.half = Vec3{50.0f, 0.5f, 50.0f};
        desc.colliders.push_back(floor);
        desc.grid.cell = 2.0f;
        desc.grid.arenaHalf = 6.0f;
    };

    // Рашер: сплошная стена башен между спавнером и ядром — прогрызает и доходит.
    {
        SceneDesc desc;
        makeArena(desc);
        BuildingSpec core;
        core.kind = BuildingSpec::Core;
        core.pos = desc.grid.cellCenter(-2, 0);
        core.hp = 500.0f;
        desc.buildings.push_back(core);
        BuildingSpec sp;
        sp.kind = BuildingSpec::Spawner;
        sp.pos = desc.grid.cellCenter(2, 0);
        sp.rate = 0.15f;
        sp.cap = 1.0f;
        desc.buildings.push_back(sp);
        int lo = 0, hi = -1;
        desc.grid.cellRange(lo, hi);
        int wallCount = 0;
        for (int cz = lo; cz <= hi; ++cz) {
            BuildingSpec tw;
            tw.kind = BuildingSpec::Tower;
            tw.pos = desc.grid.cellCenter(0, cz);
            tw.hp = 15.0f;
            tw.rate = 99.0f;
            tw.range = 0.0f;
            desc.buildings.push_back(tw);
            ++wallCount;
        }
        CharacterDesc rusher;
        rusher.goal = CharacterDesc::MobGoal::Core;
        rusher.hp = 200.0f;
        rusher.damage = 20.0f;
        rusher.speed = 5.0f;
        rusher.attackInterval = 0.15f;
        desc.enemyTypes.push_back(rusher);
        desc.enemy.hp = 200.0f;
        desc.enemy.damage = 20.0f;
        desc.enemy.attackInterval = 0.15f;

        GameWorld world;
        world.configure(desc);
        const Vec3 corePos = desc.grid.cellCenter(-2, 0);
        int towersLeft = wallCount;
        float nearestCore = 999.0f;
        bool sawDamage = false;
        for (int i = 0; i < 450; ++i) {
            world.step(kTickDt);
            std::vector<EntityState> st;
            world.writeStates(st);
            towersLeft = 0;
            nearestCore = 999.0f;
            for (const EntityState& s : st) {
                if ((EntityType)s.type == EntityType::Tower) {
                    ++towersLeft;
                    if (s.hp < 14.9f) sawDamage = true;
                }
                if ((EntityType)s.type == EntityType::Enemy) {
                    float dx = s.x - corePos.x, dz = s.z - corePos.z;
                    float d = std::sqrt(dx * dx + dz * dz);
                    if (d < nearestCore) nearestCore = d;
                }
            }
        }
        const bool wallBroken = towersLeft < wallCount || sawDamage;
        const bool reached = nearestCore < 3.0f;
        std::printf("[PathTest] рашер через стену: башен %d/%d, к ядру=%.1f, урон=%d\n",
                    towersLeft, wallCount, (double)nearestCore, (int)sawDamage);
        if (!wallBroken || !reached) {
            std::printf("[PathTest] FAIL: рашер не прогрыз стену или не дошёл\n");
            return 1;
        }
    }

    // Ломатель: путь к ядру свободен, но цель — здание; бьёт башню, ядро целёхонько.
    {
        SceneDesc desc;
        makeArena(desc);
        BuildingSpec core;
        core.kind = BuildingSpec::Core;
        core.pos = desc.grid.cellCenter(-2, 0);
        core.hp = 500.0f;
        desc.buildings.push_back(core);
        BuildingSpec tw;
        tw.kind = BuildingSpec::Tower;
        tw.pos = desc.grid.cellCenter(0, 0);
        tw.hp = 80.0f;
        tw.rate = 99.0f;
        tw.range = 0.0f;
        desc.buildings.push_back(tw);
        BuildingSpec sp;
        sp.kind = BuildingSpec::Spawner;
        sp.pos = desc.grid.cellCenter(2, 0);
        sp.rate = 0.15f;
        sp.cap = 1.0f;
        desc.buildings.push_back(sp);
        CharacterDesc br;
        br.goal = CharacterDesc::MobGoal::Building;
        br.hp = 200.0f;
        br.damage = 15.0f;
        br.speed = 5.0f;
        br.attackInterval = 0.15f;
        desc.enemyTypes.push_back(br);

        GameWorld world;
        world.configure(desc);
        for (int i = 0; i < 45; ++i) world.step(kTickDt);
        std::vector<EntityState> st;
        world.writeStates(st);
        float coreHp = 500.0f, towerHp = 80.0f;
        bool haveTower = false;
        for (const EntityState& s : st) {
            if ((EntityType)s.type == EntityType::Core) coreHp = s.hp;
            if ((EntityType)s.type == EntityType::Tower) { towerHp = s.hp; haveTower = true; }
        }
        std::printf("[PathTest] ломатель: hp башни=%.1f (ждём <80), hp ядра=%.1f (ждём ~500)\n",
                    haveTower ? (double)towerHp : 0.0, (double)coreHp);
        if (!((!haveTower || towerHp < 79.0f) && coreHp > 499.0f)) {
            std::printf("[PathTest] FAIL: ломатель пошёл не в здание\n");
            return 1;
        }
    }

    // Статические кубы (не здания) перекрывают прямую — обходят, доходят до ядра.
    {
        SceneDesc desc;
        makeArena(desc);
        BuildingSpec core;
        core.kind = BuildingSpec::Core;
        core.pos = desc.grid.cellCenter(-2, 0);
        core.hp = 500.0f;
        desc.buildings.push_back(core);
        BuildingSpec sp;
        sp.kind = BuildingSpec::Spawner;
        sp.pos = desc.grid.cellCenter(2, 0);
        sp.rate = 0.15f;
        sp.cap = 1.0f;
        desc.buildings.push_back(sp);
        for (int cz : {-1, 0, 1}) {
            ColliderSpec box;
            box.center = desc.grid.cellCenter(0, cz);
            box.center.y = 0.9f;
            box.half = Vec3{0.85f, 0.9f, 0.85f};
            desc.colliders.push_back(box);
        }
        CharacterDesc rusher;
        rusher.goal = CharacterDesc::MobGoal::Core;
        rusher.hp = 200.0f;
        rusher.damage = 20.0f;
        rusher.speed = 5.0f;
        rusher.attackInterval = 0.15f;
        desc.enemyTypes.push_back(rusher);
        desc.enemy.hp = 200.0f;
        desc.enemy.damage = 20.0f;
        desc.enemy.attackInterval = 0.15f;

        GameWorld world;
        world.configure(desc);
        const Vec3 corePos = desc.grid.cellCenter(-2, 0);
        float nearestCore = 999.0f;
        for (int i = 0; i < 250; ++i) {
            world.step(kTickDt);
            std::vector<EntityState> st;
            world.writeStates(st);
            nearestCore = 999.0f;
            for (const EntityState& s : st) {
                if ((EntityType)s.type != EntityType::Enemy) continue;
                float dx = s.x - corePos.x, dz = s.z - corePos.z;
                float d = std::sqrt(dx * dx + dz * dz);
                if (d < nearestCore) nearestCore = d;
            }
        }
        std::printf("[PathTest] обход кубов: к ядру=%.1f (ждём <3)\n", (double)nearestCore);
        if (nearestCore >= 3.0f) {
            std::printf("[PathTest] FAIL: застряли на статических кубах\n");
            return 1;
        }
    }

    std::printf("[PathTest] OK\n");
    return 0;
}

// Бесконечные волны: спавнер с waveSize>0 не иссякает и растит волны; легаси (waveSize=0)
// стопается на cap. Враги здесь бессмертны и безвредны — считаем только СКОЛЬКО породилось.
int runEndlessWaveTest() {
    auto makeBase = [](SceneDesc& desc) {
        ColliderSpec floor;
        floor.center = Vec3{0.0f, -0.5f, 0.0f};
        floor.half = Vec3{50.0f, 0.5f, 50.0f};
        desc.colliders.push_back(floor);
        desc.grid.cell = 2.0f;
        desc.grid.arenaHalf = 20.0f;
        BuildingSpec core;
        core.kind = BuildingSpec::Core;
        core.pos = desc.grid.cellCenter(-6, 0);
        core.hp = 1e7f;  // не умирает -> нет матч-рестарта, счётчик врагов монотонен
        desc.buildings.push_back(core);
        CharacterDesc slow;  // бессмертный, безвредный, медленный — просто копится
        slow.goal = CharacterDesc::MobGoal::Core;
        slow.hp = 1e6f; slow.damage = 0.0f; slow.speed = 0.5f; slow.attackInterval = 1.0f;
        desc.enemyTypes.push_back(slow);
        desc.enemy.hp = 1e6f; desc.enemy.damage = 0.0f; desc.enemy.attackInterval = 1.0f;
    };
    auto countEnemies = [](GameWorld& w) {
        std::vector<EntityState> st; w.writeStates(st);
        int n = 0; for (const EntityState& s : st) if ((EntityType)s.type == EntityType::Enemy) ++n;
        return n;
    };
    const int steps = (int)(4.0f / kTickDt);  // ~4 c симуляции

    // Бесконечный: волны 4,6,8,10... через rate=0.1 с паузой 0.5 → за ~4 c ~24 врага
    // (больше легаси-cap 12, но меньше сплошного потока 40 из-за пауз).
    {
        SceneDesc desc; makeBase(desc);
        BuildingSpec sp;
        sp.kind = BuildingSpec::Spawner;
        sp.pos = desc.grid.cellCenter(6, 0);
        sp.rate = 0.1f; sp.waveSize = 4; sp.wavePause = 0.5f; sp.waveGrow = 2;
        desc.buildings.push_back(sp);
        GameWorld world; world.configure(desc);
        for (int i = 0; i < steps; ++i) world.step(kTickDt);
        int n = countEnemies(world);
        std::printf("[WaveTest] бесконечный: врагов за ~4с=%d (ждём >13 и <37: растущие волны+паузы)\n", n);
        if (n <= 13 || n >= 37) { std::printf("[WaveTest] FAIL: волны иссякли/не растут/сплошной поток\n"); return 1; }
    }
    // Легаси: waveSize=0, cap=3 → ровно 3 и стоп
    {
        SceneDesc desc; makeBase(desc);
        BuildingSpec sp;
        sp.kind = BuildingSpec::Spawner;
        sp.pos = desc.grid.cellCenter(6, 0);
        sp.rate = 0.1f; sp.cap = 3;  // waveSize=0 по умолчанию
        desc.buildings.push_back(sp);
        GameWorld world; world.configure(desc);
        for (int i = 0; i < steps; ++i) world.step(kTickDt);
        int n = countEnemies(world);
        std::printf("[WaveTest] легаси: врагов=%d (ждём 3 = cap, стоп)\n", n);
        if (n != 3) { std::printf("[WaveTest] FAIL: легаси-cap сломан\n"); return 1; }
    }
    std::printf("[WaveTest] OK\n");
    return 0;
}

// Плотная застройка (vbase_server --densetest, отдельно от --selftest): маршрутизация
// через узкие 1-клеточные проходы, полностью запечатанное ядро (фолбэк «нет пути → ломать»),
// диагональный зазор (запрет срезания угла), детектор застреваний и стоимость пересчёта
// поля на большой сетке. Наглядная замена визуальной проверке (окно агент не видит).
int runDenseBuildTest() {
    int fails = 0;

    auto arena = [](SceneDesc& desc, float half) {
        ColliderSpec floor;
        floor.center = Vec3{0.0f, -0.5f, 0.0f};
        floor.half = Vec3{half + 4.0f, 0.5f, half + 4.0f};
        desc.colliders.push_back(floor);
        desc.grid.cell = 2.0f;
        desc.grid.arenaHalf = half;
    };
    auto addB = [](SceneDesc& desc, BuildingSpec::Kind k, int cx, int cz, float hp,
                   float range = 0.0f) {
        BuildingSpec b;
        b.kind = k;
        b.pos = desc.grid.cellCenter(cx, cz);
        b.hp = hp;
        b.range = range;   // 0 -> башня как чистая стена (не стреляет)
        b.rate = 99.0f;
        desc.buildings.push_back(b);
    };
    auto rusher = [](SceneDesc& desc, float hp, float dmg, float spd, float atkint) {
        CharacterDesc r;
        r.goal = CharacterDesc::MobGoal::Core;
        r.hp = hp;
        r.damage = dmg;
        r.speed = spd;
        r.attackInterval = atkint;
        desc.enemyTypes.push_back(r);
        desc.enemy.hp = hp;
        desc.enemy.damage = dmg;
        desc.enemy.attackInterval = atkint;
    };

    // Прогон мира с детектором застреваний. Возвращает: ближайший подход врага к ядру,
    // макс. серию тиков «стоит на месте И не атакует» (застрял), сколько тиков враг был
    // в reachDist от ядра (дошёл), был ли хоть какой-то урон по постройкам.
    struct RunStats { float nearestCore; int maxStuck; int reachedTicks; bool anyDamage; };
    auto run = [](GameWorld& world, Vec3 corePos, int ticks, float reachDist) -> RunStats {
        std::unordered_map<uint32_t, std::pair<float, float>> lastPos;
        std::unordered_map<uint32_t, int> stuckStreak;
        RunStats rs{999.0f, 0, 0, false};
        float prevBuildHp = -1.0f;
        for (int i = 0; i < ticks; ++i) {
            world.step(kTickDt);
            std::vector<EntityState> st;
            world.writeStates(st);
            float nearest = 999.0f, buildHp = 0.0f;
            for (const EntityState& s : st) {
                EntityType t = (EntityType)s.type;
                if (t == EntityType::Enemy) {
                    float dx = s.x - corePos.x, dz = s.z - corePos.z;
                    float d = std::sqrt(dx * dx + dz * dz);
                    if (d < nearest) nearest = d;
                    auto it = lastPos.find(s.id);
                    bool moved = (it == lastPos.end());
                    if (it != lastPos.end()) {
                        float mdx = s.x - it->second.first, mdz = s.z - it->second.second;
                        moved = (mdx * mdx + mdz * mdz) > (0.01f * 0.01f);
                    }
                    lastPos[s.id] = {s.x, s.z};
                    if (!moved && s.attackT <= 0.5f) {
                        int v = ++stuckStreak[s.id];
                        if (v > rs.maxStuck) rs.maxStuck = v;
                    } else {
                        stuckStreak[s.id] = 0;
                    }
                } else if (t == EntityType::Tower || t == EntityType::Core ||
                           t == EntityType::Generator || t == EntityType::Storage) {
                    buildHp += s.hp;
                }
            }
            if (nearest < rs.nearestCore) rs.nearestCore = nearest;
            if (nearest < reachDist) rs.reachedTicks++;
            if (prevBuildHp >= 0.0f && buildHp < prevBuildHp - 0.001f) rs.anyDamage = true;
            prevBuildHp = buildHp;
        }
        return rs;
    };

    // --- S1: серпантин из 1-клеточных проходов (маршрутизация через тесноту) ---
    {
        const float half = 24.0f;
        SceneDesc desc;
        arena(desc, half);
        const int lo = -12, hi = 11;  // диапазон клеток при arena 24 / cell 2
        addB(desc, BuildingSpec::Core, -10, 0, 100000.0f);
        BuildingSpec sp;
        sp.kind = BuildingSpec::Spawner;
        sp.pos = desc.grid.cellCenter(10, 0);
        sp.rate = 0.2f;
        sp.cap = 1.0f;
        desc.buildings.push_back(sp);
        // 4 стены поперёк, зазор по одной клетке, попеременно верх/низ.
        const int wallX[4] = {8, 4, 0, -4};
        for (int w = 0; w < 4; ++w) {
            int gap = (w % 2 == 0) ? hi : lo;  // чётные — сверху, нечётные — снизу
            for (int cz = lo; cz <= hi; ++cz) {
                if (cz == gap) continue;
                addB(desc, BuildingSpec::Tower, wallX[w], cz, 100000.0f);  // несокрушимая стена
            }
        }
        rusher(desc, 100000.0f, 10.0f, 5.0f, 0.2f);  // толстый — не гибнет, просто идёт

        GameWorld world;
        world.configure(desc);
        RunStats rs = run(world, desc.grid.cellCenter(-10, 0), 2500, 3.0f);
        std::printf("[Dense] S1 серпантин: подход к ядру=%.2f (ждём <3), застрял_макс=%d тик, "
                    "дошёл_тиков=%d\n",
                    (double)rs.nearestCore, rs.maxStuck, rs.reachedTicks);
        if (rs.nearestCore >= 3.0f || rs.maxStuck > 300) {
            std::printf("[Dense] FAIL: не прошёл серпантин или застрял\n");
            ++fails;
        }
    }

    // --- S2: полностью запечатанное ядро (кольцо построек) — фолбэк-снос, прорыв ---
    {
        const float half = 14.0f;
        SceneDesc desc;
        arena(desc, half);
        addB(desc, BuildingSpec::Core, 0, 0, 100000.0f);
        // Кольцо 1: 8 клеток вокруг ядра, скромный hp — рашер прогрызает одну и входит.
        for (int dx = -1; dx <= 1; ++dx)
            for (int dz = -1; dz <= 1; ++dz)
                if (dx != 0 || dz != 0) addB(desc, BuildingSpec::Tower, dx, dz, 60.0f);
        BuildingSpec sp;
        sp.kind = BuildingSpec::Spawner;
        sp.pos = desc.grid.cellCenter(6, 0);
        sp.rate = 0.2f;
        sp.cap = 1.0f;
        desc.buildings.push_back(sp);
        rusher(desc, 500.0f, 30.0f, 5.0f, 0.15f);

        GameWorld world;
        world.configure(desc);
        RunStats rs = run(world, desc.grid.cellCenter(0, 0), 1500, 3.0f);
        std::printf("[Dense] S2 запечатанное ядро: урон_по_стене=%d, прорвался(подход=%.2f), "
                    "застрял_макс=%d\n",
                    (int)rs.anyDamage, (double)rs.nearestCore, rs.maxStuck);
        if (!rs.anyDamage || rs.nearestCore >= 3.0f) {
            std::printf("[Dense] FAIL: не прогрыз кольцо или не дошёл до ядра\n");
            ++fails;
        }
    }

    // --- S3: ядро запечатано, «щель» только диагональная — срезать угол нельзя, значит
    //         рашер обязан ЛОМАТЬ (а не проскользнуть по диагонали между двумя башнями) ---
    {
        const float half = 14.0f;
        SceneDesc desc;
        arena(desc, half);
        addB(desc, BuildingSpec::Core, 0, 0, 100000.0f);
        // 8 клеток вокруг ядра, но (1,1) ОТКРЫТА. Ядро всё равно запечатано: попасть в (1,1)
        // из ядра можно лишь срезав угол между (1,0) и (0,1) — а это запрещено. Правильное
        // поведение: toCore недостижимо -> рашер ломает башню и прорывается (урон обязателен).
        for (int dx = -1; dx <= 1; ++dx)
            for (int dz = -1; dz <= 1; ++dz) {
                if (dx == 0 && dz == 0) continue;
                if (dx == 1 && dz == 1) continue;  // диагональная щель
                addB(desc, BuildingSpec::Tower, dx, dz, 60.0f);
            }
        BuildingSpec sp;
        sp.kind = BuildingSpec::Spawner;
        sp.pos = desc.grid.cellCenter(6, 6);  // заходит со стороны открытой диагонали
        sp.rate = 0.2f;
        sp.cap = 1.0f;
        desc.buildings.push_back(sp);
        rusher(desc, 500.0f, 30.0f, 5.0f, 0.15f);

        GameWorld world;
        world.configure(desc);
        RunStats rs = run(world, desc.grid.cellCenter(0, 0), 1500, 3.0f);
        // Ключевое: дошёл до ядра (<3) И для этого ЛОМАЛ (anyDamage). Если бы угол срезался —
        // рашер проскользнул бы диагональ без урона (anyDamage=false) либо застрял бы в углу.
        std::printf("[Dense] S3 диагональ-щель: ломал=%d, прорвался(подход=%.2f), застрял_макс=%d\n",
                    (int)rs.anyDamage, (double)rs.nearestCore, rs.maxStuck);
        if (!rs.anyDamage || rs.nearestCore >= 3.0f || rs.maxStuck > 300) {
            std::printf("[Dense] FAIL: рашер проскользнул диагональ, застрял или не прорвался\n");
            ++fails;
        }
    }

    // --- S4: стоимость ОДНОГО пересчёта на большой плотной сетке (= спайк на смену построек) ---
    {
        Grid g;
        g.cell = 2.0f;
        g.arenaHalf = 400.0f;  // ~400 клеток на сторону, как в scale-тесте
        NavGrid nav;
        nav.reset(g);
        const int side = nav.width();
        // Плотный блок футпринтов ~20x20 = 400 «зданий» в центре.
        auto rasterizeAll = [&]() {
            nav.clearBlocked();
            for (int cx = -10; cx < 10; ++cx)
                for (int cz = -10; cz < 10; ++cz)
                    nav.rasterizeBox(Vec3{(cx + 0.5f) * g.cell, 0.5f, (cz + 0.5f) * g.cell},
                                     Vec3{g.cell * 0.5f, 0.5f, g.cell * 0.5f});
        };
        const int reps = 5;
        auto t0 = std::chrono::steady_clock::now();
        FlowField f;
        for (int r = 0; r < reps; ++r) {
            rasterizeAll();                         // растеризация занятости
            f.compute(nav, {NavCell{side / 2 - 10, side / 2 - 10}});  // 1 BFS по всему полю
        }
        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / reps;
        // Оценка полного пересчёта на смену построек: 2 поля (toCore+toBuildings) на команду
        // с мобами. В PvE это ×2 к измеренному (ленивый eval считает только team 0).
        std::printf("[Dense] S4 пересчёт (сетка %d^2=%d кл, 400 футпринтов): rasterize+BFS=%.2f мс; "
                    "полный ребилд PvE ~%.2f мс (2 поля)\n",
                    side, side * side, ms, ms * 2.0);
        if (ms > 50.0) {  // порог-страховка: даже 512^2 не должен уходить в десятки мс на поле
            std::printf("[Dense] WARN: пересчёт поля дороговат (%.2f мс) — см. #1 (ленивость уже вкл.)\n",
                        ms);
        }
    }

    std::printf("[Dense] %s (провалов: %d)\n", fails == 0 ? "OK" : "FAIL", fails);
    return fails == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);  // консоль читает наш UTF-8, иначе кириллица — каша
#endif
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // логи сервера — сразу, без буфера
    std::signal(SIGINT, onStopSignal);
    std::signal(SIGTERM, onStopSignal);
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
#endif

    if (argc > 1 && std::strcmp(argv[1], "--densetest") == 0) {
        return runDenseBuildTest();     // плотная застройка: серпантин/запечатка/диагональ/перф
    }

    if (argc > 1 && std::strcmp(argv[1], "--selftest") == 0) {
        int a = runPhysicsSelfTest();   // примитив: collide-and-slide
        int b = runServerPathTest();    // серверный путь: сеть -> авторитет с коллизиями
        int c = runEconomyTest();       // экономика: генератор -> ресурс с потолком
        int d = runSpawnerTest();       // спавнеры -> враги бегут к ядру
        int e = runCombatTest();        // бой: враги валят ядро / башни валят врагов + фаза матча
        int f = runBuildTest();         // стройка: MSG_BUILD -> валидация на сетке -> сущность
        int g = runTeamEconomyTest();   // ресурс per-team: независимые пулы + трата пула команды
        int h = runCoopTest();          // кооп: 2 клиента на одной базе + дисконнект
        int k = runHeroStakesTest();    // ставки героя: урон от врагов -> повержен -> респаун
        int m = runTeamCombatTest();    // team-aware бой: свои не бьют своих, чужих бьют (PvP)
        int n = runPvpTest();           // PvP: назначение стороны + спавн-точки + per-team исход
        int o = runMatchRestartTest();  // матч-рестарт: decided -> авто-пересбор -> новый матч
        int p = runPathfindTest();      // поле потока: стена/фолбэк + ломатель + сетка 400
        int q = runEndlessWaveTest();   // бесконечные волны растут; легаси-спавнер стопается на cap
        int r = runHeroAttackTest();    // авто-атака героя: melee/LOS-стена/маг-снаряды
        return (a == 0 && b == 0 && c == 0 && d == 0 && e == 0 && f == 0 && g == 0 && h == 0 &&
                k == 0 && m == 0 && n == 0 && o == 0 && p == 0 && q == 0 && r == 0) ? 0 : 1;
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
    if (runningInDocker()) {
        std::printf("Docker: с хоста Join на 127.0.0.1:%u, с телефона — на LAN-IP машины (UDP).\n",
                    (unsigned)port);
        std::printf("        Адреса контейнера (172.x) клиентам не подходят.\n");
    }

    // Мир коллизий: грузим ТО ЖЕ описание сцены, что и клиент (та же геометрия —
    // иначе предсказание у стены разойдётся с авторитетом). Аргументы (как у десктопа):
    // argv[2] = каталог assets, argv[3] = путь сцены.
    std::string assetsDir;
    if (argc > 2) {
        assetsDir = argv[2];
    } else if (runningInDocker()) {
        assetsDir = "/app/assets";
    } else {
        assetsDir = "../../app/src/main/assets";
    }
    const char* scenePath = (argc > 3) ? argv[3] : "scenes/default.scene";
    FileAssetSource assets(assetsDir);
    SceneDesc desc;
    if (loadSceneDesc(assets, scenePath, desc)) {
        BuildingConfig cfg;  // параметры зданий (rate/cap/…) — из конфига
        loadBuildingConfig(assets, "config/buildings.cfg", cfg);
        applyBuildingConfig(desc, cfg);
        loadCharacterRoster(assets, "config/enemies.cfg", desc.enemyTypes);  // статы типов мобов
        loadCharacterRoster(assets, "config/characters.cfg", desc.heroTypes);  // статы героев (hp/speed)
        server.configureWorld(desc);
    } else {
        std::fprintf(stderr, "Сцена не загружена (%s, assets=%s) — сервер без коллизий\n",
                     scenePath, assetsDir.c_str());
    }

    const double tick = kTickDt;  // единый шаг симуляции (из engine/net/Net.h)
    auto last = std::chrono::steady_clock::now();
    double accumulator = 0.0;
    int lastClients = -1;

    while (gRunning.load()) {
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

    server.stop();
    std::printf("VBase server: остановлен\n");
    return 0;
}
