#include "Character.h"

#include <cmath>

namespace {
float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
}  // namespace

void Character::update(float dt, Vec3 moveDir, float mag, bool faceMove) {
    // Сглаживаем скорость (плавный разгон/торможение).
    float target = clamp01(mag);
    speed01 += (target - speed01) * clamp01(dt * 10.0f);

    if (mag > 0.05f) {
        position = position + moveDir * (mag * maxSpeed * dt);
        // Доворачиваемся к направлению движения только вперёд/вбок; назад —
        // пятимся, сохраняя ориентацию (иначе камера-follow даёт вращение).
        if (faceMove) {
            float desired = std::atan2(moveDir.x, moveDir.z);
            facingYaw = lerpAngle(facingYaw, desired, clamp01(dt * turnRate));
        }
    }

    // Параметр локомоции: 0 стоим -> 1 идём -> 2 бежим. Шаг держится до
    // половины скорости, бег включается только на сильном отклонении вперёд.
    float targetL = (speed01 < 0.02f) ? 0.0f : (1.0f + clamp01((speed01 - 0.5f) / 0.5f));
    animParam += (targetL - animParam) * clamp01(dt * 6.0f);

    animTime += dt;
}

SkinnedItem Character::buildItem() const {
    SkinnedItem item;
    item.mesh = mesh;
    item.texture = tex;
    item.color = (tex != 0) ? Vec3{1.0f, 1.0f, 1.0f} : Vec3{0.85f, 0.5f, 0.25f};
    item.model = Mat4::translation(position)
               * Mat4::rotationY(facingYaw + modelYawOffset)
               * Mat4::scale({scale, scale, scale});

    if (model != nullptr && !model->animations.empty()) {
        int n = (int)model->animations.size();
        auto id = [n](int i) { return i < n ? i : n - 1; };
        // Fox: 0=Survey(idle), 1=Walk, 2=Run. Блендим по animParam.
        if (animParam <= 0.01f) {
            model->sampleAnimation(id(0), animTime, item.joints);
        } else if (animParam <= 1.0f) {
            model->sampleBlend(id(0), animTime, id(1), animTime, animParam, item.joints);
        } else {
            model->sampleBlend(id(1), animTime, id(2), animTime, animParam - 1.0f, item.joints);
        }
    }
    return item;
}
