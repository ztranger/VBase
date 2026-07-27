#pragma once

#include "MathUtil.h"

// Орбитальная камера: смотрит на target, крутится вокруг него (yaw/pitch)
// на расстоянии distance. Всё в игровом слое — рендер камеру не трогает,
// а получает уже готовые матрицы через RenderFrame.
struct Camera {
    Vec3 target{0.0f, 0.5f, 0.0f};
    float distance = 7.0f;
    float yaw = 0.7f;    // радианы, поворот вокруг вертикали
    float pitch = 0.5f;  // радианы, наклон
    float fovY = 1.0f;   // ~57°
    float nearZ = 0.1f;
    float farZ = 100.0f;

    Vec3 eye() const {
        float cp = std::cos(pitch), sp = std::sin(pitch);
        return {target.x + distance * cp * std::cos(yaw),
                target.y + distance * sp,
                target.z + distance * cp * std::sin(yaw)};
    }

    Mat4 view() const { return Mat4::lookAt(eye(), target, {0.0f, 1.0f, 0.0f}); }
    Mat4 proj(float aspect) const { return Mat4::perspective(fovY, aspect, nearZ, farZ); }
};
