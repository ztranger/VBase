#pragma once

#include <cmath>

#include "engine/core/MathUtil.h"

// Камера ¾-вида: висит над целью под ФИКСИРОВАННЫМ наклоном (pitch) и смотрит на неё
// сверху-сбоку. Азимут (yaw) и зум (distance) управляются ИГРОКОМ (правый стик/стрелки),
// а не разворотом актора. Про «лису» ничего не знает — Scene каждый кадр передаёт позицию
// цели, а поворот/зум камеры двигает отдельными вызовами.
struct FollowCamera {
    float yaw = 0.0f;         // орбитальный азимут камеры (управляется игроком)
    float pitch = 0.9f;       // ФИКСИРОВАННЫЙ наклon над горизонтом, рад (~51° = ¾-вид)
    float distance = 16.0f;   // «наклонная» дистанция от цели до камеры
    float lookHeight = 1.0f;  // куда смотреть относительно цели (чуть выше центра)
    float fovY = 0.9f;
    float nearZ = 0.1f;
    float farZ = 200.0f;
    float minDistance = 6.0f;
    float maxDistance = 34.0f;

    Vec3 focus{0.0f, 0.0f, 0.0f};  // сглаженная точка, на которую смотрим
    bool inited = false;

    // Следуем только за ПОЗИЦИЕЙ цели (yaw/зум управляются игроком, не целью).
    void follow(Vec3 targetPos, float dt) {
        if (!inited) {
            focus = targetPos;
            inited = true;
            return;
        }
        float kp = dt * 8.0f;
        if (kp > 1.0f) kp = 1.0f;  // сглаженно догоняем позицию
        focus = focus + (targetPos - focus) * kp;
    }

    void rotate(float dYaw) { yaw += dYaw; }
    void zoom(float dDist) {
        distance += dDist;
        if (distance < minDistance) distance = minDistance;
        if (distance > maxDistance) distance = maxDistance;
    }

    // «Вперёд» камеры на плоскости земли (для camera-relative движения героя).
    Vec3 forward() const { return {std::sin(yaw), 0.0f, std::cos(yaw)}; }

    Vec3 eye() const {
        // Позади цели по forward (горизонтальная составляющая) и выше (вертикальная) —
        // так «вверх на стике» = «от камеры вперёд», код движения не меняется.
        float horiz = distance * std::cos(pitch);
        float vert = distance * std::sin(pitch);
        return focus - forward() * horiz + Vec3{0.0f, vert, 0.0f};
    }

    Mat4 view() const {
        return Mat4::lookAt(eye(), focus + Vec3{0.0f, lookHeight, 0.0f}, {0.0f, 1.0f, 0.0f});
    }
    Mat4 proj(float aspect) const {
        return Mat4::perspective(fovY, aspect, nearZ, farZ);
    }
};
