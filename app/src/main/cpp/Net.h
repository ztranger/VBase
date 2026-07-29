#pragma once

#include <cstdint>
#include <vector>

#include "Input.h"

struct SceneDesc;  // описание сцены (коллайдеры + спавн) — из SceneDesc.h

// Порт по умолчанию.
constexpr uint16_t kNetPort = 7777;

// Состояние одной сущности в снапшоте (то, что сервер шлёт клиентам).
struct EntityState {
    uint32_t id = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float yaw = 0.0f;
    float animParam = 0.0f;
    float speed01 = 0.0f;
};

// Клиент: подключается к серверу, шлёт InputCommand, принимает снапшоты.
// ENet спрятан за pimpl, чтобы не тащить его заголовки в остальной код.
class NetClient {
public:
    NetClient();
    ~NetClient();
    NetClient(const NetClient&) = delete;
    NetClient& operator=(const NetClient&) = delete;

    bool connect(const char* host, uint16_t port);
    void disconnect();
    bool connected() const;
    uint32_t myId() const;              // 0, пока не пришёл Welcome

    void sendInput(const InputCommand& cmd);
    void poll();                        // прокачать сеть, разобрать сообщения
    bool consumeSnapshot();             // true если пришёл новый снапшот (сбрасывает флаг)
    uint32_t ackSeq() const;            // последний обработанный сервером seq (для сверки)
    const std::vector<EntityState>& states() const;

private:
    struct Impl;
    Impl* impl_;
};

// Авторитетный сервер: симулирует по одному Character на клиента и рассылает
// снапшоты. Опрашивается из главного цикла (без потоков).
class NetServer {
public:
    NetServer();
    ~NetServer();
    NetServer(const NetServer&) = delete;
    NetServer& operator=(const NetServer&) = delete;

    bool start(uint16_t port);
    void stop();
    bool running() const;

    // Построить мир коллизий из описания сцены (коллайдеры + спавн игрока). Вызывать
    // после start. Без него сервер симулирует без коллизий (fallback). Та же геометрия,
    // что у клиента, — иначе предсказание у стены будет расходиться с авторитетом.
    void configureWorld(const SceneDesc& desc);

    void poll();          // принять подключения и входящий ввод
    void tick(float dt);  // симулировать всех + разослать снапшот
    int clientCount() const;

private:
    struct Impl;
    Impl* impl_;
};
