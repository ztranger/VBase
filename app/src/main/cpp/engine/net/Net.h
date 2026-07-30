#pragma once

#include <cstdint>
#include <vector>

#include "engine/core/Input.h"

struct SceneDesc;  // описание сцены (коллайдеры + спавн) — из SceneDesc.h

// Порт по умолчанию.
constexpr uint16_t kNetPort = 7777;

// Тип игровой сущности (тег в снапшоте; по нему клиент выбирает визуал/поведение).
// Пока используется только Hero; остальные — задел под геймплей (генераторы,
// хранилища, спавнеры, враги, башни, ядро базы).
enum class EntityType : uint8_t {
    Hero = 0,
    Generator,
    Storage,
    Spawner,
    Enemy,
    Tower,
    Core,
};

// Состояние одной сущности в снапшоте (то, что сервер шлёт клиентам). Обобщено под
// систему сущностей: тип + команда + generic-слоты (hp / aux — ресурс/прогресс/…).
struct EntityState {
    uint32_t id = 0;
    uint8_t type = 0;      // EntityType
    uint8_t team = 0;      // 0 = нейтрал/PvE; 1/2 — стороны (кооп/PvP)
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float yaw = 0.0f;
    float animParam = 0.0f;
    float speed01 = 0.0f;
    float velY = 0.0f;     // вертикальная скорость (для реконсиляции прыжка на клиенте)
    float hp = 0.0f;       // здоровье (герой/враг/здание); 0 = не используется
    float aux = 0.0f;      // generic-слот по типу: ресурс в хранилище, прогресс, …
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
    void tick(float dt);  // симулировать всех + разослать дельта-снапшот
    int clientCount() const;
    int debugLastChanged() const;   // changedCount последнего снапшота (для самотеста дельты)
    float debugResource() const;    // общий пул ресурса базы (для самотеста экономики)
    int debugEnemyCount() const;    // число живых врагов (для самотеста спавна)

private:
    struct Impl;
    Impl* impl_;
};
