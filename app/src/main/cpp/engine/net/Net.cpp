#include "engine/net/Net.h"

#include <enet/enet.h>

#include <cmath>
#include <cstring>
#include <memory>

#include "game/Character.h"
#include "engine/physics/CollisionWorld.h"
#include "engine/core/Log.h"
#include "game/SceneDesc.h"

namespace {

// --- Протокол (POD, фиксированная раскладка; клиенты одной ABI) ---
enum : uint8_t { MSG_WELCOME = 1, MSG_INPUT = 2, MSG_SNAPSHOT = 3 };

#pragma pack(push, 1)
struct WelcomeMsg {
    uint8_t type;
    uint32_t entityId;
};
struct InputMsg {
    uint8_t type;
    uint32_t seq;
    float moveX, moveZ, magnitude;
    uint8_t faceMove;
    uint8_t jump;
    uint32_t ackTick;  // последний применённый клиентом снапшот (база для дельты)
};
// Дельта-снапшот: изменения относительно снапшота baseTick, который клиент подтвердил.
// baseTick=0 — полный снапшот (база пуста). Далее: changedCount×EntityState (новые/
// изменившиеся), затем removedCount×uint32_t (id исчезнувших сущностей).
struct SnapshotHeader {
    uint8_t type;
    uint32_t serverTick;    // тик этого снапшота
    uint32_t baseTick;      // тик базы (0 = полный)
    uint32_t ackSeq;        // последний обработанный ввод (для реконсиляции)
    uint16_t changedCount;
    uint16_t removedCount;
};
#pragma pack(pop)

// Параметры врагов (G2): скорость бега, капсула, дистанция «дошёл до ядра».
constexpr float kEnemySpeed = 3.0f;
constexpr float kEnemyRadius = 0.3f;
constexpr float kEnemyCylHalf = 0.3f;
constexpr float kCoreStopDist = 1.0f;

// Снимок состояния мира на конкретном тике (кольцо истории на обеих сторонах).
constexpr uint32_t kSnapHistory = 64;  // ~2 c при 30 Гц
struct SnapshotRecord {
    uint32_t tick = 0;
    std::vector<EntityState> states;
};

// Сущность «изменилась», если любое поле отличается заметно (эпсилон гасит
// асимптотический дозвон speed01/animParam — иначе дельта не схлопывалась бы).
bool stateChanged(const EntityState& a, const EntityState& b) {
    const float e = 1e-3f;
    return a.type != b.type || a.team != b.team ||
           std::fabs(a.x - b.x) > e || std::fabs(a.y - b.y) > e ||
           std::fabs(a.z - b.z) > e || std::fabs(a.yaw - b.yaw) > e ||
           std::fabs(a.animParam - b.animParam) > e || std::fabs(a.speed01 - b.speed01) > e ||
           std::fabs(a.velY - b.velY) > e ||
           std::fabs(a.hp - b.hp) > e || std::fabs(a.aux - b.aux) > e;
}

const EntityState* findState(const std::vector<EntityState>& v, uint32_t id) {
    for (const EntityState& s : v) if (s.id == id) return &s;
    return nullptr;
}

// Единичная инициализация ENet на процесс.
bool ensureEnet() {
    static bool ok = false;
    static bool tried = false;
    if (!tried) {
        tried = true;
        ok = (enet_initialize() == 0);
        if (!ok) LOGE("enet_initialize failed");
    }
    return ok;
}

}  // namespace

// ============================ NetClient ============================

struct NetClient::Impl {
    ENetHost* host = nullptr;
    ENetPeer* peer = nullptr;
    uint32_t myId = 0;
    bool connected = false;
    bool newSnapshot = false;
    uint32_t ackSeq = 0;
    std::vector<EntityState> states;   // текущее полное состояние (реконструированное)
    uint32_t stateTick = 0;            // тик текущего состояния (его подтверждаем серверу)
    SnapshotRecord recvHistory[kSnapHistory];  // кольцо принятых снапшотов (базы для дельт)
};

NetClient::NetClient() : impl_(new Impl()) {}
NetClient::~NetClient() {
    disconnect();
    delete impl_;
}

bool NetClient::connect(const char* host, uint16_t port) {
    if (!ensureEnet()) return false;
    disconnect();
    impl_->host = enet_host_create(nullptr, 1, 2, 0, 0);
    if (impl_->host == nullptr) {
        LOGE("enet_host_create (client) failed");
        return false;
    }
    ENetAddress addr;
    enet_address_set_host(&addr, host);
    addr.port = port;
    impl_->peer = enet_host_connect(impl_->host, &addr, 2, 0);
    if (impl_->peer == nullptr) {
        LOGE("enet_host_connect failed");
        return false;
    }
    LOGI("NetClient: подключение к %s:%u", host, port);
    return true;
}

void NetClient::disconnect() {
    if (impl_->peer != nullptr) {
        enet_peer_disconnect_now(impl_->peer, 0);
        impl_->peer = nullptr;
    }
    if (impl_->host != nullptr) {
        enet_host_destroy(impl_->host);
        impl_->host = nullptr;
    }
    impl_->connected = false;
    impl_->myId = 0;
    impl_->states.clear();
    impl_->stateTick = 0;
    for (SnapshotRecord& r : impl_->recvHistory) { r.tick = 0; r.states.clear(); }
}

bool NetClient::connected() const { return impl_->connected; }
uint32_t NetClient::myId() const { return impl_->myId; }
uint32_t NetClient::ackSeq() const { return impl_->ackSeq; }
const std::vector<EntityState>& NetClient::states() const { return impl_->states; }

bool NetClient::consumeSnapshot() {
    bool v = impl_->newSnapshot;
    impl_->newSnapshot = false;
    return v;
}

void NetClient::sendInput(const InputCommand& cmd) {
    if (impl_->peer == nullptr || !impl_->connected) return;
    InputMsg msg{};
    msg.type = MSG_INPUT;
    msg.seq = cmd.seq;
    msg.moveX = cmd.moveX;
    msg.moveZ = cmd.moveZ;
    msg.magnitude = cmd.magnitude;
    msg.faceMove = cmd.faceMove ? 1 : 0;
    msg.jump = cmd.jump ? 1 : 0;
    msg.ackTick = impl_->stateTick;  // подтверждаем последний применённый снапшот
    // Обычный ввод — ненадёжно (realtime). Прыжок — надёжно, чтобы не потерять
    // одноразовое событие (иначе клиент подпрыгнет в предсказании, а сервер — нет).
    uint32_t flags = cmd.jump ? ENET_PACKET_FLAG_RELIABLE : 0;
    ENetPacket* pkt = enet_packet_create(&msg, sizeof(msg), flags);
    enet_peer_send(impl_->peer, 0, pkt);
}

void NetClient::poll() {
    if (impl_->host == nullptr) return;
    ENetEvent ev;
    while (enet_host_service(impl_->host, &ev, 0) > 0) {
        switch (ev.type) {
            case ENET_EVENT_TYPE_CONNECT:
                impl_->connected = true;
                LOGI("NetClient: подключён");
                break;
            case ENET_EVENT_TYPE_RECEIVE: {
                const uint8_t* data = ev.packet->data;
                size_t len = ev.packet->dataLength;
                if (len >= 1 && data[0] == MSG_WELCOME && len >= sizeof(WelcomeMsg)) {
                    WelcomeMsg w;
                    std::memcpy(&w, data, sizeof(w));
                    impl_->myId = w.entityId;
                } else if (len >= sizeof(SnapshotHeader) && data[0] == MSG_SNAPSHOT) {
                    SnapshotHeader h;
                    std::memcpy(&h, data, sizeof(h));
                    size_t need = sizeof(SnapshotHeader) +
                                  (size_t)h.changedCount * sizeof(EntityState) +
                                  (size_t)h.removedCount * sizeof(uint32_t);
                    if (len >= need) {
                        // Читаем тело через memcpy (за packed-заголовком выравнивания нет).
                        std::vector<EntityState> changed(h.changedCount);
                        if (h.changedCount)
                            std::memcpy(changed.data(), data + sizeof(SnapshotHeader),
                                        (size_t)h.changedCount * sizeof(EntityState));
                        std::vector<uint32_t> removed(h.removedCount);
                        if (h.removedCount)
                            std::memcpy(removed.data(),
                                        data + sizeof(SnapshotHeader) +
                                            (size_t)h.changedCount * sizeof(EntityState),
                                        (size_t)h.removedCount * sizeof(uint32_t));

                        // Реконструируем полное состояние на serverTick из базы + дельты.
                        std::vector<EntityState> ns;
                        bool applied = false;
                        if (h.baseTick == 0) {
                            ns = std::move(changed);  // полный снапшот
                            applied = true;
                        } else {
                            SnapshotRecord& base = impl_->recvHistory[h.baseTick % kSnapHistory];
                            if (base.tick == h.baseTick) {
                                ns = base.states;
                                for (uint32_t rid : removed)
                                    for (size_t j = 0; j < ns.size(); ++j)
                                        if (ns[j].id == rid) { ns.erase(ns.begin() + (long)j); break; }
                                for (const EntityState& cs : changed) {
                                    bool found = false;
                                    for (EntityState& s : ns)
                                        if (s.id == cs.id) { s = cs; found = true; break; }
                                    if (!found) ns.push_back(cs);
                                }
                                applied = true;
                            }
                            // базы нет (потеряли цепочку) — ждём полный от сервера
                        }

                        if (applied && h.serverTick > impl_->stateTick) {
                            SnapshotRecord& rec = impl_->recvHistory[h.serverTick % kSnapHistory];
                            rec.tick = h.serverTick;
                            rec.states = std::move(ns);
                            impl_->states = rec.states;  // копия для чтения Scene
                            impl_->stateTick = h.serverTick;
                            impl_->ackSeq = h.ackSeq;
                            impl_->newSnapshot = true;
                        }
                    }
                }
                enet_packet_destroy(ev.packet);
                break;
            }
            case ENET_EVENT_TYPE_DISCONNECT:
                impl_->connected = false;
                LOGI("NetClient: отключён");
                break;
            default:
                break;
        }
    }
}

// ============================ NetServer ============================

namespace {
// Игровая сущность на сервере (авторитетная). Движение/трансформ — в `move`
// (Character): для героя это тот же код, что предсказывает клиент; для будущих типов
// (здания/враги) — своя логика систем, а `move.position/facingYaw` служат трансформом.
struct Entity {
    uint32_t id = 0;
    EntityType type = EntityType::Hero;
    uint8_t team = 0;
    Character move;       // трансформ + (для подвижных) физика/анимация
    InputCommand input;   // герой: последний ввод (иначе не используется)
    float hp = 0.0f, maxHp = 0.0f;
    float aux = 0.0f;     // сеть: ресурс в хранилище / прогресс / …
    // Серверные параметры/рантайм (не в сети — фиксированы сценой или считаются здесь).
    float rate = 0.0f;    // generator: ресурс/сек; spawner: интервал спавна, сек
    float cap = 0.0f;     // storage: ёмкость; spawner: макс. врагов
    float timer = 0.0f;   // spawner: накопитель времени до следующего спавна
    int spawnedCount = 0; // spawner: сколько врагов уже породил (потолок = cap)
};
// Подключение клиента: peer, id управляемого им героя, подтверждённый снапшот (база дельт).
struct Conn {
    ENetPeer* peer = nullptr;
    uint32_t heroId = 0;
    uint32_t ackTick = 0;
};
}  // namespace

struct NetServer::Impl {
    ENetHost* host = nullptr;
    uint32_t nextEntityId = 1;
    uint32_t tickCount = 0;
    std::vector<Entity> entities;  // ВСЕ сущности мира (герои + позже здания/враги)
    std::vector<Conn> conns;       // подключённые клиенты
    std::vector<uint8_t> scratch;  // буфер снапшота
    SnapshotRecord history[kSnapHistory];  // кольцо разосланных состояний (базы для дельт)
    int lastChanged = 0;                   // changedCount последнего снапшота (для самотеста)

    // Мир коллизий (та же геометрия, что у клиента). Может быть пуст — тогда fallback.
    std::unique_ptr<CollisionWorld> world;
    Vec3 spawnPos{0.0f, 0.0f, 0.0f};
    float capsuleRadius = 0.3f;
    float capsuleCylHalf = 0.3f;

    float resource = 0.0f;  // общий пул ресурса базы (team 0; per-team — в кооп/PvP)

    Conn* connByPeer(ENetPeer* p) {
        for (auto& c : conns) if (c.peer == p) return &c;
        return nullptr;
    }
    Entity* entityById(uint32_t id) {
        for (auto& e : entities) if (e.id == id) return &e;
        return nullptr;
    }
};

NetServer::NetServer() : impl_(new Impl()) {}
NetServer::~NetServer() {
    stop();
    delete impl_;
}

bool NetServer::start(uint16_t port) {
    if (!ensureEnet()) return false;
    stop();
    ENetAddress addr;
    addr.host = ENET_HOST_ANY;
    addr.port = port;
    impl_->host = enet_host_create(&addr, 16, 2, 0, 0);
    if (impl_->host == nullptr) {
        LOGE("enet_host_create (server) failed");
        return false;
    }
    LOGI("NetServer: слушаю порт %u", port);
    return true;
}

void NetServer::stop() {
    if (impl_->host != nullptr) {
        enet_host_destroy(impl_->host);
        impl_->host = nullptr;
    }
    impl_->entities.clear();
    impl_->conns.clear();
    impl_->world.reset();  // сносим мир (контроллеры клиентов уйдут вместе с ним)
    impl_->nextEntityId = 1;
    for (SnapshotRecord& r : impl_->history) { r.tick = 0; r.states.clear(); }
}

void NetServer::configureWorld(const SceneDesc& desc) {
    impl_->world = std::make_unique<CollisionWorld>();
    for (const ColliderSpec& cs : desc.colliders) {
        impl_->world->addBox(cs.center, cs.half);
    }
    impl_->world->finalize();
    impl_->spawnPos = desc.player.pos;
    impl_->capsuleRadius = desc.player.colliderRadius;
    impl_->capsuleCylHalf = desc.player.colliderCylHalf;

    // Спавним статичные сущности базы из описания сцены (генератор/хранилище/спавнер/ядро).
    impl_->resource = 0.0f;
    for (const BuildingSpec& b : desc.buildings) {
        Entity e;
        e.id = impl_->nextEntityId++;
        switch (b.kind) {
            case BuildingSpec::Generator: e.type = EntityType::Generator; break;
            case BuildingSpec::Storage:   e.type = EntityType::Storage;   break;
            case BuildingSpec::Spawner:   e.type = EntityType::Spawner;   break;
            case BuildingSpec::Core:      e.type = EntityType::Core;      break;
        }
        e.move.position = b.pos;
        e.move.snapshot();
        e.rate = b.rate;
        e.cap = b.cap;
        impl_->entities.push_back(e);
    }
    LOGI("NetServer: мир — %d коллайдеров, %d сущностей базы", (int)desc.colliders.size(),
         (int)desc.buildings.size());
}

bool NetServer::running() const { return impl_->host != nullptr; }
int NetServer::clientCount() const { return (int)impl_->conns.size(); }

void NetServer::poll() {
    if (impl_->host == nullptr) return;
    ENetEvent ev;
    while (enet_host_service(impl_->host, &ev, 0) > 0) {
        switch (ev.type) {
            case ENET_EVENT_TYPE_CONNECT: {
                // Создаём сущность-героя для клиента (авторитетная позиция + контроллер).
                Entity hero;
                hero.id = impl_->nextEntityId++;
                hero.type = EntityType::Hero;
                hero.move.position = impl_->spawnPos;
                hero.move.snapshot();
                if (impl_->world) {
                    hero.move.collider = impl_->world->addCharacter(
                        impl_->spawnPos, impl_->capsuleRadius, impl_->capsuleCylHalf);
                }
                impl_->entities.push_back(hero);
                impl_->conns.push_back(Conn{ev.peer, hero.id, 0});
                WelcomeMsg w{MSG_WELCOME, hero.id};
                ENetPacket* pkt = enet_packet_create(&w, sizeof(w), ENET_PACKET_FLAG_RELIABLE);
                enet_peer_send(ev.peer, 0, pkt);
                LOGI("NetServer: клиент подключён (hero id=%u), всего %d", hero.id,
                     (int)impl_->conns.size());
                break;
            }
            case ENET_EVENT_TYPE_RECEIVE: {
                if (ev.packet->dataLength >= sizeof(InputMsg) &&
                    ev.packet->data[0] == MSG_INPUT) {
                    InputMsg m;
                    std::memcpy(&m, ev.packet->data, sizeof(m));
                    Conn* conn = impl_->connByPeer(ev.peer);
                    if (conn != nullptr) {
                        conn->ackTick = m.ackTick;  // база для дельты этому клиенту
                        Entity* hero = impl_->entityById(conn->heroId);
                        if (hero != nullptr) {
                            hero->input.seq = m.seq;
                            hero->input.moveX = m.moveX;
                            hero->input.moveZ = m.moveZ;
                            hero->input.magnitude = m.magnitude;
                            hero->input.faceMove = (m.faceMove != 0);
                            hero->input.jump = (m.jump != 0);
                        }
                    }
                }
                enet_packet_destroy(ev.packet);
                break;
            }
            case ENET_EVENT_TYPE_DISCONNECT: {
                Conn* conn = impl_->connByPeer(ev.peer);
                if (conn != nullptr) {
                    uint32_t heroId = conn->heroId;
                    LOGI("NetServer: клиент (hero id=%u) отключён", heroId);
                    Entity* hero = impl_->entityById(heroId);
                    if (hero != nullptr && impl_->world && hero->move.collider != 0) {
                        impl_->world->removeCharacter(hero->move.collider);
                    }
                    for (size_t i = 0; i < impl_->entities.size(); ++i)
                        if (impl_->entities[i].id == heroId) {
                            impl_->entities.erase(impl_->entities.begin() + (long)i);
                            break;
                        }
                    for (size_t i = 0; i < impl_->conns.size(); ++i)
                        if (impl_->conns[i].peer == ev.peer) {
                            impl_->conns.erase(impl_->conns.begin() + (long)i);
                            break;
                        }
                }
                break;
            }
            default:
                break;
        }
    }
}

void NetServer::tick(float dt) {
    if (impl_->host == nullptr) return;
    impl_->tickCount++;

    // Системы сущностей. Пока только герои: движение по вводу через общий мир коллизий
    // (тот же код, что предсказывает клиент). Дальше здесь появятся генераторы (ресурс),
    // спавнеры, движение врагов, бой.
    for (Entity& e : impl_->entities) {
        if (e.type == EntityType::Hero) {
            e.move.snapshot();
            e.move.simulate(dt, e.input, impl_->world.get());
            e.input.jump = false;  // прыжок одноразовый
        }
    }

    // Экономика: генераторы капают в общий пул, ограниченный суммой ёмкостей хранилищ
    // (лишнее теряется — это и есть потолок накоплений). Пул раскладываем по хранилищам
    // в поле aux (заполнение конкретного хранилища) — так он попадает в сеть.
    {
        float prod = 0.0f, cap = 0.0f;
        for (const Entity& e : impl_->entities) {
            if (e.type == EntityType::Generator) prod += e.rate;
            else if (e.type == EntityType::Storage) cap += e.cap;
        }
        impl_->resource += prod * dt;
        if (impl_->resource > cap) impl_->resource = cap;
        if (impl_->resource < 0.0f) impl_->resource = 0.0f;
        float rem = impl_->resource;
        for (Entity& e : impl_->entities) {
            if (e.type != EntityType::Storage) continue;
            float amt = rem < e.cap ? rem : e.cap;
            e.aux = amt;
            rem -= amt;
        }
    }

    // Спавнеры: по таймеру плодят врагов (до потолка cap). Новые сущности КОПИМ и
    // добавляем ПОСЛЕ цикла — push_back в impl_->entities инвалидировал бы итерацию.
    {
        std::vector<Entity> spawned;
        for (Entity& e : impl_->entities) {
            if (e.type != EntityType::Spawner) continue;
            if (e.rate <= 0.0f || e.spawnedCount >= (int)e.cap) continue;
            e.timer += dt;
            if (e.timer >= e.rate) {
                e.timer -= e.rate;
                Entity enemy;
                enemy.id = impl_->nextEntityId++;
                enemy.type = EntityType::Enemy;
                enemy.team = e.team;
                enemy.move.position = e.move.position;
                enemy.move.snapshot();
                if (impl_->world)
                    enemy.move.collider = impl_->world->addCharacter(
                        e.move.position, kEnemyRadius, kEnemyCylHalf);
                spawned.push_back(enemy);
                e.spawnedCount++;
            }
        }
        for (Entity& n : spawned) impl_->entities.push_back(std::move(n));
    }

    // Враги бегут к ядру базы (первый Core) тем же кинематическим контроллером
    // (гравитация + скольжение по стенам), что и герои.
    {
        Vec3 corePos{0.0f, 0.0f, 0.0f};
        bool haveCore = false;
        for (const Entity& e : impl_->entities)
            if (e.type == EntityType::Core) { corePos = e.move.position; haveCore = true; break; }
        if (haveCore) {
            for (Entity& e : impl_->entities) {
                if (e.type != EntityType::Enemy) continue;
                Vec3 to = corePos - e.move.position;
                to.y = 0.0f;
                float dist = std::sqrt(to.x * to.x + to.z * to.z);
                Vec3 vel{0.0f, 0.0f, 0.0f};
                if (dist > kCoreStopDist) {
                    Vec3 dir = to * (1.0f / dist);
                    vel = dir * kEnemySpeed;
                    e.move.facingYaw = std::atan2(dir.x, dir.z);
                }
                if (impl_->world && e.move.collider != 0)
                    e.move.position =
                        impl_->world->moveCharacter(e.move.collider, vel, e.move.velocityY, false, dt);
                else
                    e.move.position = e.move.position + vel * dt;
            }
        }
    }

    // Полное текущее состояние мира — из ВСЕХ сущностей.
    std::vector<EntityState> current(impl_->entities.size());
    for (size_t i = 0; i < impl_->entities.size(); ++i) {
        const Entity& e = impl_->entities[i];
        EntityState& s = current[i];
        s.id = e.id;
        s.type = (uint8_t)e.type;
        s.team = e.team;
        s.x = e.move.position.x;
        s.y = e.move.position.y;
        s.z = e.move.position.z;
        s.yaw = e.move.facingYaw;
        s.animParam = e.move.animParam;
        s.speed01 = e.move.speed01;
        s.velY = e.move.velocityY;
        s.hp = e.hp;
        s.aux = e.aux;
    }
    // В кольцо истории — база для будущих дельт (когда клиент подтвердит этот тик).
    SnapshotRecord& rec = impl_->history[impl_->tickCount % kSnapHistory];
    rec.tick = impl_->tickCount;
    rec.states = current;

    // Каждому клиенту — дельта относительно ПОДТВЕРЖДЁННОГО им снапшота (устойчиво к
    // потерям: база = его ackTick, а не последний посланный). Нет базы -> полный.
    for (Conn& conn : impl_->conns) {
        const SnapshotRecord* base = nullptr;
        if (conn.ackTick != 0) {
            SnapshotRecord& h = impl_->history[conn.ackTick % kSnapHistory];
            if (h.tick == conn.ackTick) base = &h;  // база ещё в истории
        }

        std::vector<EntityState> changed;
        std::vector<uint32_t> removed;
        uint32_t baseTick;
        if (base == nullptr) {
            changed = current;  // полный снапшот
            baseTick = 0;
        } else {
            baseTick = conn.ackTick;
            for (const EntityState& cur : current) {
                const EntityState* prev = findState(base->states, cur.id);
                if (prev == nullptr || stateChanged(cur, *prev)) changed.push_back(cur);
            }
            for (const EntityState& prev : base->states)
                if (findState(current, prev.id) == nullptr) removed.push_back(prev.id);
        }
        impl_->lastChanged = (int)changed.size();

        // ackSeq — последний обработанный ввод героя этого клиента (для реконсиляции).
        Entity* hero = impl_->entityById(conn.heroId);
        uint32_t ackSeq = hero ? hero->input.seq : 0;

        // Сериализация: заголовок + changed[] + removed[] (через memcpy — packed).
        size_t size = sizeof(SnapshotHeader) + changed.size() * sizeof(EntityState) +
                      removed.size() * sizeof(uint32_t);
        impl_->scratch.resize(size);
        SnapshotHeader head;
        head.type = MSG_SNAPSHOT;
        head.serverTick = impl_->tickCount;
        head.baseTick = baseTick;
        head.ackSeq = ackSeq;
        head.changedCount = (uint16_t)changed.size();
        head.removedCount = (uint16_t)removed.size();
        std::memcpy(impl_->scratch.data(), &head, sizeof(head));
        if (!changed.empty())
            std::memcpy(impl_->scratch.data() + sizeof(head), changed.data(),
                        changed.size() * sizeof(EntityState));
        if (!removed.empty())
            std::memcpy(impl_->scratch.data() + sizeof(head) + changed.size() * sizeof(EntityState),
                        removed.data(), removed.size() * sizeof(uint32_t));
        ENetPacket* pkt = enet_packet_create(impl_->scratch.data(), size, 0);
        enet_peer_send(conn.peer, 0, pkt);
    }
}

int NetServer::debugLastChanged() const { return impl_->lastChanged; }
float NetServer::debugResource() const { return impl_->resource; }
int NetServer::debugEnemyCount() const {
    int n = 0;
    for (const Entity& e : impl_->entities) if (e.type == EntityType::Enemy) ++n;
    return n;
}
