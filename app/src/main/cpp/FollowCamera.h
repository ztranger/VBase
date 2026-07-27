#pragma once

#include <cmath>

#include "MathUtil.h"

// Камера третьего лица: следует за ЛЮБОЙ целью (позиция + разворот), висит
// позади и смотрит на неё. Про «лису» ничего не знает — Scene каждый кадр
// передаёт ей позицию и facing управляемого актора.
struct FollowCamera {
    float yaw = 0.0f;         // сглаженный азимут (следует за facing цели)
    float distance = 6.0f;    // как далеко позади
    float height = 3.0f;      // как высоко над целью
    float lookHeight = 1.0f;  // куда смотреть относительно цели (чуть выше центра)
    float fovY = 1.0f;
    float nearZ = 0.1f;
    float farZ = 200.0f;

    Vec3 focus{0.0f, 0.0f, 0.0f};  // сглаженная точка, на которую смотрим
    bool inited = false;

    void follow(Vec3 targetPos, float targetYaw, float dt) {
        if (!inited) {
            focus = targetPos;
            yaw = targetYaw;
            inited = true;
            return;
        }
        float kp = dt * 8.0f;  if (kp > 1.0f) kp = 1.0f;   // догоняем позицию
        float ky = dt * 6.0f;  if (ky > 1.0f) ky = 1.0f;   // догоняем разворот
        focus = focus + (targetPos - focus) * kp;
        yaw = lerpAngle(yaw, targetYaw, ky);
    }

    Vec3 forward() const { return {std::sin(yaw), 0.0f, std::cos(yaw)}; }

    Vec3 eye() const {
        return focus + Vec3{0.0f, height, 0.0f} - forward() * distance;
    }

    Mat4 view() const {
        return Mat4::lookAt(eye(), focus + Vec3{0.0f, lookHeight, 0.0f}, {0.0f, 1.0f, 0.0f});
    }
    Mat4 proj(float aspect) const {
        return Mat4::perspective(fovY, aspect, nearZ, farZ);
    }
};
