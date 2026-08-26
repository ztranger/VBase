#pragma once

#include <cmath>
#include <cstdint>

// Команда ввода за один тик симуляции. Именно ЭТО (а не готовую позицию) клиент
// будет применять локально и слать на сервер. Пока используется локально.
struct InputCommand {
    uint32_t seq = 0;         // номер команды (для сверки с сервером в будущем)
    float moveX = 0.0f;       // нормированное направление в мире, ось X
    float moveZ = 0.0f;       // нормированное направление в мире, ось Z
    float magnitude = 0.0f;   // величина 0..1 (уже с учётом «назад медленнее»)
    bool faceMove = true;     // доворачиваться к движению (false = пятиться)
    bool jump = false;        // запрос прыжка на этом тике (срабатывает только с земли)
    bool attack = false;      // запрос атаки (каста) на этом тике — одноразовое событие
};

// Виртуальный джойстик: появляется в точке касания, вектор от центра к пальцу
// даёт направление и величину (0..1). Чисто входные данные, без рендера.
struct VirtualJoystick {
    bool active = false;
    float ox = 0.0f, oy = 0.0f;  // центр (точка нажатия), пиксели
    float cx = 0.0f, cy = 0.0f;  // текущая точка пальца
    float radius = 120.0f;       // радиус хода в пикселях (масштабируется по DPI)

    float x = 0.0f, y = 0.0f;    // нормированное направление [-1..1], y вверх
    float mag = 0.0f;            // величина отклонения 0..1

    void onPointer(float px, float py, bool pressed) {
        if (pressed) {
            if (!active) {  // палец коснулся — ставим центр сюда
                active = true;
                ox = px;
                oy = py;
            }
            cx = px;
            cy = py;
            float dx = cx - ox;
            float dy = cy - oy;
            float len = std::sqrt(dx * dx + dy * dy);
            float clamped = len > radius ? radius : len;
            if (len > 0.0001f) {
                x = (dx / len) * (clamped / radius);
                y = -(dy / len) * (clamped / radius);  // экран Y вниз -> вверх +
            } else {
                x = 0.0f;
                y = 0.0f;
            }
            mag = clamped / radius;
        } else {
            active = false;
            x = y = mag = 0.0f;
        }
    }
};
