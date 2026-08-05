#include "engine/net/Net.h"

#include <enet/enet.h>

#include <cmath>
#include <cstring>
#include <memory>

#include "engine/core/Log.h"
#include "game/GameWorld.h"  // авторитетная игровая симуляция (сущности + системы)

namespace {

// --- Протокол (POD, фиксированная раскладка; клиенты одной ABI) ---
enum : uint8_t { MSG_WELCOME = 1, MSG_INPUT = 2, MSG_SNAPSHOT = 3, MSG_BUILD = 4 };

#pragma pack(push, 1)
struct WelcomeMsg {
    uint8_t type;
    uint32_t protocolVersion;  // сервер шлёт свою версию; клиент сверяет с kProtocolVersion
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
// Запрос постройки: тип здания + клетка сетки. Надёжно (дискретное событие).
struct BuildMsg {
    uint8_t type;       // MSG_BUILD
    uint8_t buildType;  // EntityType возводимого здания
    int32_t cellX, cellZ;
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
    uint8_t phase;          // GamePhase матча (глобально, не per-entity)
};
#pragma pack(pop)

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

// enet_peer_send при ошибке (<0) НЕ освобождает пакет — освобождаем сами, иначе течёт.
void sendPacket(ENetPeer* peer, uint8_t channel, ENetPacket* pkt) {
    if (enet_peer_send(peer, channel, pkt) < 0) enet_packet_destroy(pkt);
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
    uint8_t gamePhase = 0;  // GamePhase из последнего снапшота
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
    // Версию протокола передаём как connect-data: сервер сверит её ещё на CONNECT,
    // до спавна героя, и отклонит несовместимый билд.
    impl_->peer = enet_host_connect(impl_->host, &addr, 2, kProtocolVersion);
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
    impl_->gamePhase = 0;
    impl_->states.clear();
    impl_->stateTick = 0;
    for (SnapshotRecord& r : impl_->recvHistory) { r.tick = 0; r.states.clear(); }
}

bool NetClient::connected() const { return impl_->connected; }
uint32_t NetClient::myId() const { return impl_->myId; }
uint32_t NetClient::ackSeq() const { return impl_->ackSeq; }
uint8_t NetClient::gamePhase() const { return impl_->gamePhase; }
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
    sendPacket(impl_->peer, 0, pkt);
}

void NetClient::sendBuild(uint8_t buildType, int cellX, int cellZ) {
    if (impl_->peer == nullptr || !impl_->connected) return;
    BuildMsg msg{MSG_BUILD, buildType, (int32_t)cellX, (int32_t)cellZ};
    // Надёжно: постройка — одноразовое событие, терять нельзя.
    ENetPacket* pkt = enet_packet_create(&msg, sizeof(msg), ENET_PACKET_FLAG_RELIABLE);
    sendPacket(impl_->peer, 0, pkt);
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
                    if (w.protocolVersion != kProtocolVersion) {
                        LOGE("NetClient: версия протокола сервера %u != нашей %u — отключаюсь",
                             (unsigned)w.protocolVersion, (unsigned)kProtocolVersion);
                        enet_packet_destroy(ev.packet);
                        disconnect();
                        return;  // peer/host уничтожены — выходим из poll
                    }
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
                            impl_->gamePhase = h.phase;  // фаза матча из заголовка
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
// Подключение клиента: peer, id управляемого им героя, подтверждённый снапшот (база дельт).
// Игровые сущности живут в GameWorld — сервер здесь лишь транспорт.
struct Conn {
    ENetPeer* peer = nullptr;
    uint32_t heroId = 0;
    uint32_t ackTick = 0;
};
}  // namespace

struct NetServer::Impl {
    ENetHost* host = nullptr;
    uint32_t tickCount = 0;
    GameWorld game;                // авторитетная игровая симуляция (сущности + физика, без сети)
    std::vector<Conn> conns;       // подключённые клиенты
    std::vector<uint8_t> scratch;  // буфер снапшота
    SnapshotRecord history[kSnapHistory];  // кольцо разосланных состояний (базы для дельт)
    int lastChanged = 0;                   // changedCount последнего снапшота (для самотеста)

    Conn* connByPeer(ENetPeer* p) {
        for (auto& c : conns) if (c.peer == p) return &c;
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
    impl_->game.reset();   // снести сущности + мир коллизий (контроллеры уйдут вместе с ним)
    impl_->conns.clear();
    for (SnapshotRecord& r : impl_->history) { r.tick = 0; r.states.clear(); }
}

void NetServer::configureWorld(const SceneDesc& desc) { impl_->game.configure(desc); }

bool NetServer::running() const { return impl_->host != nullptr; }
int NetServer::clientCount() const { return (int)impl_->conns.size(); }

void NetServer::poll() {
    if (impl_->host == nullptr) return;
    ENetEvent ev;
    while (enet_host_service(impl_->host, &ev, 0) > 0) {
        switch (ev.type) {
            case ENET_EVENT_TYPE_CONNECT: {
                // Версия протокола пришла как connect-data — отклоняем несовместимый билд
                // ДО спавна героя (иначе рассинхрон раскладки = порча памяти в снапшотах).
                if (ev.data != kProtocolVersion) {
                    LOGW("NetServer: отклонён клиент — версия протокола %u != %u",
                         (unsigned)ev.data, (unsigned)kProtocolVersion);
                    enet_peer_disconnect_now(ev.peer, 0);
                    break;
                }
                // Создаём авторитетную сущность-героя (позиция + контроллер) в игровом мире.
                uint32_t heroId = impl_->game.addPlayer();  // авто-выбор стороны (PvP-баланс)
                impl_->conns.push_back(Conn{ev.peer, heroId, 0});
                WelcomeMsg w{MSG_WELCOME, kProtocolVersion, heroId};
                ENetPacket* pkt = enet_packet_create(&w, sizeof(w), ENET_PACKET_FLAG_RELIABLE);
                sendPacket(ev.peer, 0, pkt);
                LOGI("NetServer: клиент подключён (hero id=%u), всего %d", heroId,
                     (int)impl_->conns.size());
                break;
            }
            case ENET_EVENT_TYPE_RECEIVE: {
                const uint8_t msgType = ev.packet->dataLength >= 1 ? ev.packet->data[0] : 0;
                if (msgType == MSG_INPUT && ev.packet->dataLength >= sizeof(InputMsg)) {
                    InputMsg m;
                    std::memcpy(&m, ev.packet->data, sizeof(m));
                    Conn* conn = impl_->connByPeer(ev.peer);
                    if (conn != nullptr) {
                        conn->ackTick = m.ackTick;  // база для дельты этому клиенту
                        InputCommand cmd;
                        cmd.seq = m.seq;
                        cmd.moveX = m.moveX;
                        cmd.moveZ = m.moveZ;
                        cmd.magnitude = m.magnitude;
                        cmd.faceMove = (m.faceMove != 0);
                        cmd.jump = (m.jump != 0);
                        impl_->game.setHeroInput(conn->heroId, cmd);
                    }
                } else if (msgType == MSG_BUILD && ev.packet->dataLength >= sizeof(BuildMsg)) {
                    BuildMsg m;
                    std::memcpy(&m, ev.packet->data, sizeof(m));
                    Conn* conn = impl_->connByPeer(ev.peer);
                    if (conn != nullptr)  // валидацию (клетка/ресурс/границы) делает GameWorld
                        impl_->game.tryBuild(conn->heroId, (EntityType)m.buildType, m.cellX, m.cellZ);
                }
                enet_packet_destroy(ev.packet);
                break;
            }
            case ENET_EVENT_TYPE_DISCONNECT: {
                Conn* conn = impl_->connByPeer(ev.peer);
                if (conn != nullptr) {
                    uint32_t heroId = conn->heroId;
                    LOGI("NetServer: клиент (hero id=%u) отключён", heroId);
                    impl_->game.removeEntity(heroId);  // сущность + её контроллер
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

    // Вся игровая симуляция — в GameWorld (движение/экономика/спавнеры/враги, дальше бой).
    // Сервер лишь двигает мир и сериализует его состояние.
    impl_->game.step(dt);

    // Полное текущее состояние мира — из ВСЕХ сущностей.
    std::vector<EntityState> current;
    impl_->game.writeStates(current);
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
        uint32_t ackSeq = impl_->game.inputSeq(conn.heroId);

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
        // Фаза матча — С ПЕРСПЕКТИВЫ КОМАНДЫ этого клиента (PvP: у сторон разный исход).
        head.phase = (uint8_t)impl_->game.phaseForTeam(impl_->game.teamOf(conn.heroId));
        std::memcpy(impl_->scratch.data(), &head, sizeof(head));
        if (!changed.empty())
            std::memcpy(impl_->scratch.data() + sizeof(head), changed.data(),
                        changed.size() * sizeof(EntityState));
        if (!removed.empty())
            std::memcpy(impl_->scratch.data() + sizeof(head) + changed.size() * sizeof(EntityState),
                        removed.data(), removed.size() * sizeof(uint32_t));
        ENetPacket* pkt = enet_packet_create(impl_->scratch.data(), size, 0);
        sendPacket(conn.peer, 0, pkt);
    }
}

int NetServer::debugLastChanged() const { return impl_->lastChanged; }
float NetServer::debugResource() const { return impl_->game.resource(); }
int NetServer::debugEnemyCount() const { return impl_->game.enemyCount(); }
int NetServer::debugPhase() const { return (int)impl_->game.gamePhase(); }
float NetServer::debugCoreHp() const { return impl_->game.coreHp(); }
