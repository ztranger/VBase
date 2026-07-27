// Выделенный (десктопный) сервер VBase. Использует ТОТ ЖЕ код симуляции и сети,
// что и клиент (Net.cpp / Character.cpp), но без Android/GL. Хост-режим в
// приложении больше не обязателен — можно поднять сервер отдельно.

#include <chrono>
#include <cstdio>
#include <cstring>
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

#include "Net.h"

namespace {

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
