#pragma once

#include <cstdint>

#include "Input.h"
#include "MathUtil.h"
#include "Model.h"
#include "RenderFrame.h"

// Управляемый персонаж как СЕТЕВАЯ СУЩНОСТЬ. Симуляция идёт на фиксированном
// тике (simulate), а рендер интерполирует между прошлым и текущим состоянием
// (buildItem(alpha)) — так картинка плавная при низкой частоте тика/сети.
// Модель (glTF) — это данные; сам класс не «лиса».
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

    // Предыдущее состояние (снимок прошлого тика) — для интерполяции при рендере.
    Vec3 prevPosition{0.0f, 0.0f, 0.0f};
    float prevFacingYaw = 0.0f;
    float prevAnimParam = 0.0f;
    float prevAnimTime = 0.0f;

    // Конфиг.
    float maxSpeed = 6.0f;
    float turnRate = 10.0f;
    float modelYawOffset = 0.0f;
    float scale = 0.03f;

    // Ссылки на модель.
    const SkinnedModel* model = nullptr;
    SkinnedHandle mesh = 0;
    TextureHandle tex = 0;

    // Зафиксировать текущее состояние как «предыдущее» (вызывать перед simulate).
    void snapshot();

    // Шаг симуляции на фиксированный dt по команде ввода.
    void simulate(float dt, const InputCommand& in);

    // Отрисовочный объект с интерполяцией между prev и текущим (alpha 0..1).
    SkinnedItem buildItem(float alpha) const;
};
