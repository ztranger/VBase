#include "game/ClientSession.h"

#include <cstring>

#include "engine/core/Log.h"
#include "game/SceneDesc.h"  // полный тип для server_.configureWorld

void ClientSession::host(const SceneDesc& sceneDesc) {
    leave();
    if (server_.start(kNetPort)) {
        server_.configureWorld(sceneDesc);  // тот же мир коллизий, что у клиента
        client_.connect("127.0.0.1", kNetPort);
        host_ = true;
        inputSeq_ = 0;
    }
}

void ClientSession::join(const char* ip, uint16_t port) {
    leave();
    std::strncpy(serverIp_, ip, sizeof(serverIp_) - 1);
    serverIp_[sizeof(serverIp_) - 1] = '\0';
    serverPort_ = port > 0 ? port : kNetPort;
    client_.connect(serverIp_, serverPort_);
    host_ = false;
    wantReconnect_ = true;  // при обрыве пытаемся вернуться на тот же сервер
    reconnectTimer_ = 0.0f;
    reconnectAttempts_ = 0;
    inputSeq_ = 0;
}

void ClientSession::leave() {
    client_.disconnect();
    server_.stop();
    host_ = false;
    wantReconnect_ = false;  // сознательный выход — не переподключаемся
    reconnectTimer_ = 0.0f;
}

bool ClientSession::pump(float dt) {
    // Сервер (если хостим) — тем же кодом симуляции, затем рассылка снапшотов. Порядок как был:
    // server до client (ввод, посланный этим кадром, флашится client.poll и доходит следующим).
    if (host_) {
        server_.poll();
        server_.tick(dt);
    }
    client_.poll();
    return client_.consumeSnapshot();
}

void ClientSession::reconnectTick(float dt) {
    if (!wantReconnect_) return;
    constexpr float kReconnectPeriod = 2.0f;  // сек между попытками
    reconnectTimer_ -= dt;
    if (reconnectTimer_ <= 0.0f) {
        reconnectTimer_ = kReconnectPeriod;
        ++reconnectAttempts_;
        LOGI("ClientSession: переподключение #%d к %s:%d", reconnectAttempts_, serverIp_,
             (int)serverPort_);
        client_.connect(serverIp_, serverPort_);  // connect() сам сбросит клиент
        inputSeq_ = 0;
    }
}

void ClientSession::sendInput(InputCommand& cmd) {
    cmd.seq = ++inputSeq_;
    client_.sendInput(cmd);
}
