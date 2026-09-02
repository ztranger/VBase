#pragma once

#include <cstdint>

#include "engine/core/Input.h"
#include "engine/core/MathUtil.h"

class CollisionWorld;

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
    float locoPhase = 0.0f;   // нормализованная фаза локомоции (циклы) для синхронного бленда
                              // walk<->run (клипы разной длины); ЛОКАЛЬНАЯ, не сеть/реконсиляция
    float velocityY = 0.0f;  // вертикальная скорость (гравитация/прыжок) — реконсилируется
    float attackTime = 0.0f; // остаток времени текущей атаки, сек (>0 = идёт каст) — реконсилируется

    // Предыдущее состояние (для интерполяции при рендере на клиенте).
    Vec3 prevPosition{0.0f, 0.0f, 0.0f};
    float prevFacingYaw = 0.0f;
    float prevAnimParam = 0.0f;
    float prevAnimTime = 0.0f;
    float prevLocoPhase = 0.0f;
    float prevAttackTime = 0.0f;

    // Конфиг движения.
    float maxSpeed = 6.0f;
    float turnRate = 10.0f;

    // Длительность атаки (каста), сек. ЕДИНАЯ для клиента и сервера (сервер headless и не
    // знает длину анимационного клипа — рендер масштабирует клип под это окно). Во время
    // атаки персонаж «рутится» (стоит на месте). Реконсилируется через EntityState.attackT.
    static constexpr float kAttackDuration = 0.8f;

    // Хэндл кинематического контроллера в CollisionWorld (0 — нет физики, fallback).
    uint32_t collider = 0;

    void snapshot();  // зафиксировать текущее как «предыдущее»
    // Шаг симуляции. Если задан world и есть collider — движение через collide-and-slide,
    // иначе — прямое интегрирование позиции (fallback без коллизий).
    void simulate(float dt, const InputCommand& in, CollisionWorld* world = nullptr);
};
