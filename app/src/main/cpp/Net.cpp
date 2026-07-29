#include "Net.h"

#include <enet/enet.h>

#include <cstring>
#include <memory>

#include "Character.h"
#include "CollisionWorld.h"
#include "Log.h"
#include "SceneDesc.h"

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
};
struct SnapshotHeader {
    uint8_t type;
    uint32_t serverTick;
    uint32_t ackSeq;
    uint16_t count;
};
#pragma pack(pop)

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
    std::vector<EntityState> states;
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
                    size_t need = sizeof(SnapshotHeader) + (size_t)h.count * sizeof(EntityState);
                    if (len >= need) {
                        impl_->ackSeq = h.ackSeq;
                        impl_->states.resize(h.count);
                        std::memcpy(impl_->states.data(), data + sizeof(SnapshotHeader),
                                    (size_t)h.count * sizeof(EntityState));
                        impl_->newSnapshot = true;
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
struct ServerClient {
    ENetPeer* peer = nullptr;
    uint32_t id = 0;
    Character ch;       // серверная (авторитетная) симуляция
    InputCommand input; // последний полученный ввод
};
}  // namespace

struct NetServer::Impl {
    ENetHost* host = nullptr;
    uint32_t nextId = 1;
    uint32_t tickCount = 0;
    std::vector<ServerClient> clients;
    std::vector<uint8_t> scratch;  // буфер снапшота

    // Мир коллизий (та же геометрия, что у клиента). Может быть пуст — тогда fallback.
    std::unique_ptr<CollisionWorld> world;
    Vec3 spawnPos{0.0f, 0.0f, 0.0f};
    float capsuleRadius = 0.3f;
    float capsuleCylHalf = 0.3f;

    ServerClient* byPeer(ENetPeer* p) {
        for (auto& c : clients) if (c.peer == p) return &c;
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
    impl_->clients.clear();
    impl_->world.reset();  // сносим мир (контроллеры клиентов уйдут вместе с ним)
    impl_->nextId = 1;
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
    LOGI("NetServer: мир коллизий — %d статичных коллайдеров", (int)desc.colliders.size());
}

bool NetServer::running() const { return impl_->host != nullptr; }
int NetServer::clientCount() const { return (int)impl_->clients.size(); }

void NetServer::poll() {
    if (impl_->host == nullptr) return;
    ENetEvent ev;
    while (enet_host_service(impl_->host, &ev, 0) > 0) {
        switch (ev.type) {
            case ENET_EVENT_TYPE_CONNECT: {
                ServerClient c;
                c.peer = ev.peer;
                c.id = impl_->nextId++;
                // Спавн: авторитетная позиция + кинематический контроллер в мире.
                c.ch.position = impl_->spawnPos;
                c.ch.snapshot();
                if (impl_->world) {
                    c.ch.collider = impl_->world->addCharacter(
                        impl_->spawnPos, impl_->capsuleRadius, impl_->capsuleCylHalf);
                }
                impl_->clients.push_back(c);
                WelcomeMsg w{MSG_WELCOME, c.id};
                ENetPacket* pkt = enet_packet_create(&w, sizeof(w), ENET_PACKET_FLAG_RELIABLE);
                enet_peer_send(ev.peer, 0, pkt);
                LOGI("NetServer: клиент подключён (id=%u), всего %d", c.id,
                     (int)impl_->clients.size());
                break;
            }
            case ENET_EVENT_TYPE_RECEIVE: {
                if (ev.packet->dataLength >= sizeof(InputMsg) &&
                    ev.packet->data[0] == MSG_INPUT) {
                    InputMsg m;
                    std::memcpy(&m, ev.packet->data, sizeof(m));
                    ServerClient* c = impl_->byPeer(ev.peer);
                    if (c != nullptr) {
                        c->input.seq = m.seq;
                        c->input.moveX = m.moveX;
                        c->input.moveZ = m.moveZ;
                        c->input.magnitude = m.magnitude;
                        c->input.faceMove = (m.faceMove != 0);
                        c->input.jump = (m.jump != 0);
                    }
                }
                enet_packet_destroy(ev.packet);
                break;
            }
            case ENET_EVENT_TYPE_DISCONNECT: {
                for (size_t i = 0; i < impl_->clients.size(); ++i) {
                    if (impl_->clients[i].peer == ev.peer) {
                        LOGI("NetServer: клиент id=%u отключён", impl_->clients[i].id);
                        if (impl_->world && impl_->clients[i].ch.collider != 0) {
                            impl_->world->removeCharacter(impl_->clients[i].ch.collider);
                        }
                        impl_->clients.erase(impl_->clients.begin() + i);
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

    // Авторитетная симуляция: каждый клиент — свой Character по его вводу,
    // через общий мир коллизий (тот же код, что предсказывает клиент).
    for (auto& c : impl_->clients) {
        c.ch.snapshot();
        c.ch.simulate(dt, c.input, impl_->world.get());
        c.input.jump = false;  // прыжок одноразовый: гасим после применения (не залипал между пакетами)
    }

    // Снапшот со всеми сущностями (states одинаковы для всех, ackSeq — свой).
    uint16_t count = (uint16_t)impl_->clients.size();
    size_t size = sizeof(SnapshotHeader) + (size_t)count * sizeof(EntityState);
    impl_->scratch.resize(size);
    auto* head = reinterpret_cast<SnapshotHeader*>(impl_->scratch.data());
    head->type = MSG_SNAPSHOT;
    head->serverTick = impl_->tickCount;
    head->count = count;
    auto* states = reinterpret_cast<EntityState*>(impl_->scratch.data() + sizeof(SnapshotHeader));
    for (uint16_t i = 0; i < count; ++i) {
        const Character& ch = impl_->clients[i].ch;
        states[i].id = impl_->clients[i].id;
        states[i].x = ch.position.x;
        states[i].y = ch.position.y;
        states[i].z = ch.position.z;
        states[i].yaw = ch.facingYaw;
        states[i].animParam = ch.animParam;
        states[i].speed01 = ch.speed01;
    }
    for (auto& c : impl_->clients) {
        head->ackSeq = c.input.seq;  // персонально для этого клиента
        ENetPacket* pkt = enet_packet_create(impl_->scratch.data(), size, 0);
        enet_peer_send(c.peer, 0, pkt);
    }
}
