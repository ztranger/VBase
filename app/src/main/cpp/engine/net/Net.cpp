#include "engine/net/Net.h"

#include <enet/enet.h>

#include <cmath>
#include <cstring>
#include <memory>
#include <unordered_map>

#include "engine/core/Log.h"
#include "engine/net/ByteIO.h"  // явная LE-сериализация управляющих сообщений (P2-15)
#include "game/GameWorld.h"  // авторитетная игровая симуляция (сущности + системы)

namespace {

// --- Протокол ---
enum : uint8_t {
    MSG_WELCOME = 1, MSG_INPUT = 2, MSG_SNAPSHOT = 3, MSG_BUILD = 4,
    MSG_UPGRADE = 5,   // {u32 targetId} — поднять тир башни/ловушки
    MSG_DEMOLISH = 6,  // {u32 targetId} — снести свою постройку (возврат ресурса)
};

// P2-15: управляющие сообщения (Welcome/Input/Build) сериализуются ЯВНО, побайтно в little-endian
// (ByteWriter/ByteReader, engine/net/ByteIO.h) — без зависимости от раскладки/паддинга/endianness
// и с проверкой границ при чтении. Их packed-структуры убраны.
//
// Снапшот (SnapshotHeader + EntityState[] + id[]) — высокочастотный и перф-чувствительный путь —
// ОСТАВЛЕН на прежней memcpy-раскладке, защищённой `#pragma pack(1)` + `static_assert` на
// sizeof/offsetof (ловит дрейф раскладки при сборке) + `kProtocolVersion` (несовместимый билд
// отклоняется до спавна) + гардами длины при чтении. Миграция снапшота на явную сериализацию —
// следующий шаг «по одному типу сообщения» (см. PROJECT_REVIEW P2-15), делать отдельно с бенчем.
#pragma pack(push, 1)
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
           std::fabs(a.hp - b.hp) > e || std::fabs(a.aux - b.aux) > e ||
           std::fabs(a.attackT - b.attackT) > e || a.charType != b.charType;
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
    NetStatus status = NetStatus::Offline;
    bool newSnapshot = false;
    uint8_t charType = 0;   // выбранный персонаж — шлём в каждом InputMsg
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
    // Ускоряем детект разрыва: по умолчанию ENet держит peer до 5-30 с без ответа.
    // (limit=0 -> дефолт 32; минимум 4 с, максимум 8 с) — обрыв ловится за ~4-8 с,
    // при этом кратковременный лаг-спайк не рвёт соединение. Пинги ENet шлёт сам.
    enet_peer_timeout(impl_->peer, 0, 4000, 8000);
    impl_->status = NetStatus::Connecting;
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
    impl_->status = NetStatus::Offline;
    impl_->myId = 0;
    impl_->gamePhase = 0;
    impl_->states.clear();
    impl_->stateTick = 0;
    for (SnapshotRecord& r : impl_->recvHistory) { r.tick = 0; r.states.clear(); }
}

bool NetClient::connected() const { return impl_->status == NetStatus::Connected; }
NetStatus NetClient::status() const { return impl_->status; }
int NetClient::pingMs() const {
    // roundTripTime осмыслен только на живом соединении (до CONNECT там дефолт 500 мс).
    if (impl_->status != NetStatus::Connected || impl_->peer == nullptr) return -1;
    return (int)impl_->peer->roundTripTime;
}
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
    if (impl_->peer == nullptr || impl_->status != NetStatus::Connected) return;
    ByteWriter w;  // явная LE-раскладка (P2-15), байт-в-байт как прежняя packed-структура
    w.u8(MSG_INPUT);
    w.u32(cmd.seq);
    w.f32(cmd.moveX);
    w.f32(cmd.moveZ);
    w.f32(cmd.magnitude);
    w.u8(cmd.faceMove ? 1 : 0);
    w.u8(cmd.jump ? 1 : 0);
    w.u8(cmd.attack ? 1 : 0);
    w.u8(impl_->charType);
    w.u32(impl_->stateTick);  // подтверждаем последний применённый снапшот (ackTick)
    // Обычный ввод — ненадёжно (realtime). Прыжок/атака — надёжно, чтобы не потерять
    // одноразовое событие (иначе клиент проиграет его в предсказании, а сервер — нет).
    uint32_t flags = (cmd.jump || cmd.attack) ? ENET_PACKET_FLAG_RELIABLE : 0;
    ENetPacket* pkt = enet_packet_create(w.data(), w.size(), flags);
    sendPacket(impl_->peer, 0, pkt);
}

void NetClient::setCharType(uint8_t charType) { impl_->charType = charType; }

void NetClient::sendBuild(uint8_t buildType, uint8_t kind, int cellX, int cellZ) {
    if (impl_->peer == nullptr || impl_->status != NetStatus::Connected) return;
    ByteWriter w;  // явная LE-раскладка (P2-15)
    w.u8(MSG_BUILD);
    w.u8(buildType);
    w.u8(kind);      // v7: вид башни/ловушки (для Tower/Trap; иначе сервер игнорирует)
    w.i32((int32_t)cellX);
    w.i32((int32_t)cellZ);
    // Надёжно: постройка — одноразовое событие, терять нельзя.
    ENetPacket* pkt = enet_packet_create(w.data(), w.size(), ENET_PACKET_FLAG_RELIABLE);
    sendPacket(impl_->peer, 0, pkt);
}

void NetClient::sendUpgrade(uint32_t targetId) {
    if (impl_->peer == nullptr || impl_->status != NetStatus::Connected) return;
    ByteWriter w;
    w.u8(MSG_UPGRADE);
    w.u32(targetId);
    ENetPacket* pkt = enet_packet_create(w.data(), w.size(), ENET_PACKET_FLAG_RELIABLE);
    sendPacket(impl_->peer, 0, pkt);
}

void NetClient::sendDemolish(uint32_t targetId) {
    if (impl_->peer == nullptr || impl_->status != NetStatus::Connected) return;
    ByteWriter w;
    w.u8(MSG_DEMOLISH);
    w.u32(targetId);
    ENetPacket* pkt = enet_packet_create(w.data(), w.size(), ENET_PACKET_FLAG_RELIABLE);
    sendPacket(impl_->peer, 0, pkt);
}

void NetClient::debugSendRaw(const void* data, size_t len) {
    if (impl_->peer == nullptr || impl_->status != NetStatus::Connected) return;
    // len==0 -> валидный ENet-пакет нулевой длины (проверяем гард msgType при dataLength<1).
    ENetPacket* pkt = enet_packet_create(data, len, ENET_PACKET_FLAG_RELIABLE);
    sendPacket(impl_->peer, 0, pkt);
}

void NetClient::poll() {
    if (impl_->host == nullptr) return;
    ENetEvent ev;
    while (enet_host_service(impl_->host, &ev, 0) > 0) {
        switch (ev.type) {
            case ENET_EVENT_TYPE_CONNECT:
                impl_->status = NetStatus::Connected;
                LOGI("NetClient: подключён");
                break;
            case ENET_EVENT_TYPE_RECEIVE: {
                const uint8_t* data = ev.packet->data;
                size_t len = ev.packet->dataLength;
                if (len >= 1 && data[0] == MSG_WELCOME) {
                    ByteReader rd(data, len);
                    rd.u8();  // type
                    uint32_t ver = rd.u32();
                    uint32_t entId = rd.u32();
                    if (rd.ok() && ver != kProtocolVersion) {
                        LOGE("NetClient: версия протокола сервера %u != нашей %u — отключаюсь",
                             (unsigned)ver, (unsigned)kProtocolVersion);
                        enet_packet_destroy(ev.packet);
                        disconnect();
                        return;  // peer/host уничтожены — выходим из poll
                    }
                    if (rd.ok()) impl_->myId = entId;
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
                                // Индекс id -> позиция в ns: применение дельты O(N) вместо O(N^2)
                                // (вложенные линейные поиски). Порядок ns не важен (Scene матчит
                                // по id, и это же база следующей дельты) — потому swap-and-pop.
                                std::unordered_map<uint32_t, size_t> idx;
                                idx.reserve(ns.size());
                                for (size_t j = 0; j < ns.size(); ++j) idx[ns[j].id] = j;
                                for (uint32_t rid : removed) {  // удалить исчезнувшие
                                    auto it = idx.find(rid);
                                    if (it == idx.end()) continue;
                                    size_t pos = it->second, last = ns.size() - 1;
                                    if (pos != last) { ns[pos] = ns[last]; idx[ns[pos].id] = pos; }
                                    ns.pop_back();
                                    idx.erase(it);
                                }
                                for (const EntityState& cs : changed) {  // обновить/добавить
                                    auto it = idx.find(cs.id);
                                    if (it != idx.end()) {
                                        ns[it->second] = cs;
                                    } else {
                                        idx[cs.id] = ns.size();
                                        ns.push_back(cs);
                                    }
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
                // Событие приходит только при НЕПРОШЕННОМ обрыве (наш disconnect() рвёт
                // peer через _now и обнуляет его ДО poll, так что сюда не попадает): это
                // таймаут либо закрытие сервера. Помечаем Lost — верхний слой покажет на
                // экране и запустит переподключение. Peer уже сброшен ENet — обнуляем.
                impl_->status = NetStatus::Lost;
                impl_->peer = nullptr;
                LOGI("NetClient: соединение потеряно");
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
// P2-04: rate-limit ввода/стройки per-peer. Легит-клиент шлёт ~1 ввод/тик; щедрые кэпы бьют
// только по флуду. Пакеты сверх кэпа за тик отбрасываются; устойчивый флуд рвёт peer.
constexpr uint16_t kMaxInputsPerTick = 8;   // с запасом на догон буфера ввода
constexpr uint16_t kMaxBuildsPerTick = 4;   // стройка — редкое действие игрока
constexpr uint16_t kFloodTicksLimit = 30;   // ~1 c непрерывного превышения -> дисконнект

struct Conn {
    ENetPeer* peer = nullptr;
    uint32_t heroId = 0;
    uint32_t ackTick = 0;       // подтверждённая база дельт (валидируется: монотонно, не из будущего)
    uint32_t lastFullTick = 0;  // тик последнего высланного этому peer full-снапшота (rate-limit)
    uint16_t inputsThisTick = 0;  // P2-04: счётчики за тик (сбрасываются в tick), детект флуда
    uint16_t buildsThisTick = 0;
    uint16_t floodTicks = 0;      // сколько тиков подряд упирался в кэп (устойчивый флуд)
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
    uint32_t fullSnapshots = 0;            // счётчики наблюдаемости (full vs delta) — для самотеста/метрик
    uint32_t deltaSnapshots = 0;

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
                ByteWriter w;
                w.u8(MSG_WELCOME);
                w.u32(kProtocolVersion);
                w.u32(heroId);
                ENetPacket* pkt = enet_packet_create(w.data(), w.size(), ENET_PACKET_FLAG_RELIABLE);
                sendPacket(ev.peer, 0, pkt);
                LOGI("NetServer: клиент подключён (hero id=%u), всего %d", heroId,
                     (int)impl_->conns.size());
                break;
            }
            case ENET_EVENT_TYPE_RECEIVE: {
                const uint8_t msgType = ev.packet->dataLength >= 1 ? ev.packet->data[0] : 0;
                if (msgType == MSG_INPUT) {
                    // Явный декод (P2-15): ByteReader.ok() = пакет был достаточной длины (гард
                    // усечённого/битого пакета вместо sizeof-проверки + memcpy за границу).
                    ByteReader rd(ev.packet->data, ev.packet->dataLength);
                    rd.u8();  // type
                    const uint32_t seq = rd.u32();
                    const float mx = rd.f32(), mz = rd.f32(), mag = rd.f32();
                    const uint8_t face = rd.u8(), jmp = rd.u8(), atk = rd.u8(), ct = rd.u8();
                    const uint32_t ackTick = rd.u32();
                    Conn* conn = impl_->connByPeer(ev.peer);
                    // P2-04: сверх кэпа ввода за тик — отбрасываем (флуд не грузит симуляцию).
                    if (rd.ok() && conn != nullptr && conn->inputsThisTick < kMaxInputsPerTick) {
                        ++conn->inputsThisTick;
                        // Валидируем ack ДО применения (P1-03): только монотонный и не из будущего.
                        if (ackIsValid(ackTick, conn->ackTick, impl_->tickCount))
                            conn->ackTick = ackTick;  // база для дельты этому клиенту
                        InputCommand cmd;
                        cmd.seq = seq;
                        cmd.moveX = mx;
                        cmd.moveZ = mz;
                        cmd.magnitude = mag;
                        cmd.faceMove = (face != 0);
                        cmd.jump = (jmp != 0);
                        cmd.attack = (atk != 0);
                        impl_->game.setHeroInput(conn->heroId, cmd);
                        impl_->game.setHeroCharType(conn->heroId, ct);  // выбор персонажа
                    }
                } else if (msgType == MSG_BUILD) {
                    ByteReader rd(ev.packet->data, ev.packet->dataLength);
                    rd.u8();  // type
                    const uint8_t bt = rd.u8();
                    const uint8_t kind = rd.u8();  // v7: вид башни/ловушки (Tower/Trap)
                    const int32_t cx = rd.i32(), cz = rd.i32();
                    Conn* conn = impl_->connByPeer(ev.peer);
                    // P2-04: сверх кэпа стройки за тик — отбрасываем. Валидацию (клетка/ресурс/
                    // границы/вид) делает GameWorld.
                    if (rd.ok() && conn != nullptr && conn->buildsThisTick < kMaxBuildsPerTick) {
                        ++conn->buildsThisTick;
                        impl_->game.tryBuild(conn->heroId, (EntityType)bt, kind, cx, cz);
                    }
                } else if (msgType == MSG_UPGRADE || msgType == MSG_DEMOLISH) {
                    ByteReader rd(ev.packet->data, ev.packet->dataLength);
                    rd.u8();  // type
                    const uint32_t targetId = rd.u32();
                    Conn* conn = impl_->connByPeer(ev.peer);
                    // Тот же кэп «действий стройки за тик» — от флуда апгрейдами/сносами.
                    if (rd.ok() && conn != nullptr && conn->buildsThisTick < kMaxBuildsPerTick) {
                        ++conn->buildsThisTick;
                        if (msgType == MSG_UPGRADE) impl_->game.tryUpgrade(conn->heroId, targetId);
                        else impl_->game.tryDemolish(conn->heroId, targetId);
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

    // P2-04: poll() (до tick) уже разобрал пакеты кадра и накопил счётчики. Здесь детектим
    // устойчивый флуд (упор в кэп много тиков подряд -> дисконнект) и сбрасываем счётчики на кадр.
    for (Conn& c : impl_->conns) {
        if (c.inputsThisTick >= kMaxInputsPerTick || c.buildsThisTick >= kMaxBuildsPerTick) {
            if (++c.floodTicks >= kFloodTicksLimit && c.peer != nullptr) {
                LOGW("NetServer: peer (hero id=%u) флудит вводом/стройкой — дисконнект", c.heroId);
                enet_peer_disconnect(c.peer, 0);
                c.floodTicks = 0;
            }
        } else if (c.floodTicks > 0) {
            --c.floodTicks;
        }
        c.inputsThisTick = 0;
        c.buildsThisTick = 0;
    }

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

    // Индекс id -> состояние текущего мира: O(1) поиск в дельте (было findState = O(N^2) на peer).
    std::unordered_map<uint32_t, const EntityState*> curIdx;
    curIdx.reserve(current.size());
    for (const EntityState& s : current) curIdx[s.id] = &s;
    constexpr uint32_t kFullSnapshotMinGap = 10;  // не чаще ~3 full/с на peer (антиамплификация)

    // Каждому клиенту — дельта относительно ПОДТВЕРЖДЁННОГО им снапшота (устойчиво к
    // потерям: база = его ackTick, а не последний посланный). Нет базы -> полный.
    for (Conn& conn : impl_->conns) {
        const SnapshotRecord* base = nullptr;
        if (conn.ackTick != 0) {
            SnapshotRecord& h = impl_->history[conn.ackTick % kSnapHistory];
            if (h.tick == conn.ackTick) base = &h;  // база ещё в истории
        }

        // Rate-limit full-снапшотов: базы нет (нужен full), но недавно уже слали full этому peer
        // -> пропускаем тик. Легитимный клиент получает один full и уходит на дельты; тут стопается
        // лишь тот, кто искусственно форсит full (или редкое восстановление после потери цепочки —
        // оно уложится в gap, ~0.33 c). Иначе поддельный ack давал бы 30 full/с (амплификация).
        if (base == nullptr && conn.lastFullTick != 0 &&
            impl_->tickCount - conn.lastFullTick < kFullSnapshotMinGap) {
            continue;
        }

        std::vector<EntityState> changed;
        std::vector<uint32_t> removed;
        uint32_t baseTick;
        if (base == nullptr) {
            changed = current;  // полный снапшот
            baseTick = 0;
            conn.lastFullTick = impl_->tickCount;
            ++impl_->fullSnapshots;
        } else {
            baseTick = conn.ackTick;
            std::unordered_map<uint32_t, const EntityState*> baseIdx;
            baseIdx.reserve(base->states.size());
            for (const EntityState& s : base->states) baseIdx[s.id] = &s;
            for (const EntityState& cur : current) {  // изменившиеся/новые: поиск в базе O(1)
                auto it = baseIdx.find(cur.id);
                if (it == baseIdx.end() || stateChanged(cur, *it->second)) changed.push_back(cur);
            }
            for (const EntityState& prev : base->states)  // исчезнувшие: нет в текущем
                if (curIdx.find(prev.id) == curIdx.end()) removed.push_back(prev.id);
            ++impl_->deltaSnapshots;
        }
        impl_->lastChanged = (int)changed.size();

        // Инвариант: сущностей <= kMaxEntities (< 2^16) -> счётчики влезают в uint16. Если больше
        // (не должно) — НЕ шлём битый пакет: заголовок разошёлся бы с payload = порча у клиента.
        if (changed.size() > 0xFFFFu || removed.size() > 0xFFFFu) {
            LOGE("NetServer: снапшот %zu changed / %zu removed > uint16 — пропуск (лимит мира?)",
                 changed.size(), removed.size());
            continue;
        }

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
uint32_t NetServer::debugFullSnapshots() const { return impl_->fullSnapshots; }
uint32_t NetServer::debugDeltaSnapshots() const { return impl_->deltaSnapshots; }
float NetServer::debugResource() const { return impl_->game.resource(); }
int NetServer::debugEnemyCount() const { return impl_->game.enemyCount(); }
int NetServer::debugPhase() const { return (int)impl_->game.gamePhase(); }
float NetServer::debugCoreHp() const { return impl_->game.coreHp(); }
