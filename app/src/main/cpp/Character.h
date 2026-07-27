#pragma once

#include "MathUtil.h"
#include "Model.h"
#include "RenderFrame.h"

// Управляемый персонаж: позиция/разворот + локомоция + анимация от скорости.
// Модель (glTF) — это данные, которые в него кладут; сам класс не «лиса».
struct Character {
    // Состояние.
    Vec3 position{0.0f, 0.0f, 0.0f};
    float facingYaw = 0.0f;   // куда «смотрит» (радианы)
    float speed01 = 0.0f;     // текущая норм. скорость 0..1 (сглажена)
    float animParam = 0.0f;   // 0=idle, 1=walk, 2=run (сглажен)
    float animTime = 0.0f;

    // Конфиг.
    float maxSpeed = 6.0f;       // мировых единиц/сек при полном отклонении
    float turnRate = 10.0f;      // скорость доворота к направлению движения
    float modelYawOffset = 0.0f; // подгонка «морда по движению» (зависит от модели)
    float scale = 0.03f;

    // Ссылки на модель.
    const SkinnedModel* model = nullptr;
    SkinnedHandle mesh = 0;
    TextureHandle tex = 0;

    // moveDir — нормированное горизонтальное направление в мире (y=0), mag 0..1.
    // faceMove — доворачиваться ли мордой к направлению движения (false = пятиться).
    void update(float dt, Vec3 moveDir, float mag, bool faceMove);

    // Собрать объект для рендера (с семплингом анимации по animParam).
    SkinnedItem buildItem() const;
};
