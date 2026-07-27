#pragma once

#include <cstdint>

#include "Input.h"
#include "MathUtil.h"

// Управляемый персонаж — ЧИСТАЯ СИМУЛЯЦИЯ (без рендера и модели), чтобы этот
// код компилировался и на клиенте (Android/GL), и на выделенном сервере.
// Отрисовку (модель, скиннинг) строит клиентский слой по состоянию Character.
struct Character {
    enum class Owner { Local, Remote };

    uint32_t entityId = 0;
    Owner owner = Owner::Local;

    // Текущее состояние симуляции.
    Vec3 position{0.0f, 0.0f, 0.0f};
    float facingYaw = 0.0f;
    float speed01 = 0.0f;
    float animParam = 0.0f;
    float animTime = 0.0f;

    // Предыдущее состояние (для интерполяции при рендере на клиенте).
    Vec3 prevPosition{0.0f, 0.0f, 0.0f};
    float prevFacingYaw = 0.0f;
    float prevAnimParam = 0.0f;
    float prevAnimTime = 0.0f;

    // Конфиг движения.
    float maxSpeed = 6.0f;
    float turnRate = 10.0f;

    void snapshot();  // зафиксировать текущее как «предыдущее»
    void simulate(float dt, const InputCommand& in);  // шаг симуляции
};
