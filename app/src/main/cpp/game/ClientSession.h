#pragma once

#include <cstdint>
#include <vector>

#include "engine/core/Input.h"  // InputCommand
#include "engine/net/Net.h"     // NetClient, NetServer, NetStatus, EntityState, kNetPort

struct SceneDesc;  // game/SceneDesc.h (host отдаёт его встроенному серверу)

// P2-18 шаг 3: сетевая СЕССИЯ клиента — транспорт (`NetClient`), встроенный host-сервер
// (`NetServer`), лайфсайкл host/join/leave и авто-реконнект. Знает только про транспорт: не
// трогает рендер/мир/предсказание. Владеет `client_`/`server_` (раньше — прямо в Scene).
// Scene дёргает её (`pump`/`sendInput`/…) и применяет снапшоты (`states()`), а мировое
// состояние (remoteEntities и пр.) чистит сам.
class ClientSession {
public:
    // Host: поднять локальный сервер из sceneDesc и подключиться к нему (127.0.0.1).
    void host(const SceneDesc& sceneDesc);
    // Join: подключиться к ip:port (запоминает адрес для авто-реконнекта).
    void join(const char* ip, uint16_t port);
    // Leave: отключиться + остановить host-сервер + выключить реконнект (транспорт).
    void leave();

    // Пампинг за тик: host -> server poll+tick; всегда client poll. true = пришёл новый снапшот
    // (Scene должен применить его applySnapshot()). Порядок как раньше: server до client.
    bool pump(float dt);
    // Авто-реконнект (звать при status()==Lost): раз в период connect на запомненный ip:port.
    void reconnectTick(float dt);

    // Аплинк. sendInput проставляет seq (inputSeq_) в cmd И отправляет (cmd меняется — по ссылке).
    void sendInput(InputCommand& cmd);
    void sendBuild(uint8_t buildType, int cellX, int cellZ) { client_.sendBuild(buildType, cellX, cellZ); }
    void setCharType(uint8_t charType) { client_.setCharType(charType); }

    // Даунлинк / состояние (для мира и UI).
    bool connected() const { return client_.connected(); }
    NetStatus status() const { return client_.status(); }
    bool isHost() const { return host_; }
    int pingMs() const { return client_.pingMs(); }
    uint32_t myId() const { return client_.myId(); }
    const std::vector<EntityState>& states() const { return client_.states(); }
    uint32_t ackSeq() const { return client_.ackSeq(); }
    uint8_t gamePhase() const { return client_.gamePhase(); }

    // Реконнект-UI.
    const char* serverAddress() const { return serverIp_; }
    int serverPort() const { return (int)serverPort_; }
    int reconnectAttempts() const { return reconnectAttempts_; }
    void retryNow() { reconnectTimer_ = 0.0f; }

private:
    NetClient client_;
    NetServer server_;
    bool host_ = false;
    // Авто-реконнект для join-сессии (host к 127.0.0.1 не переподключаем). serverIp_ запоминается
    // в join; при статусе Lost повторяем connect раз в kReconnectPeriod.
    char serverIp_[64] = {0};
    uint16_t serverPort_ = kNetPort;
    int reconnectAttempts_ = 0;
    bool wantReconnect_ = false;
    float reconnectTimer_ = 0.0f;
    uint32_t inputSeq_ = 0;
};
