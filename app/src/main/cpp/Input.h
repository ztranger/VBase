#pragma once

#include <cmath>

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
