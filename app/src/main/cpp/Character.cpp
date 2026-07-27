#include "Character.h"

#include <cmath>

namespace {
float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
}  // namespace

void Character::snapshot() {
    prevPosition = position;
    prevFacingYaw = facingYaw;
    prevAnimParam = animParam;
    prevAnimTime = animTime;
}

void Character::simulate(float dt, const InputCommand& in) {
    float mag = in.magnitude;
    Vec3 moveDir{in.moveX, 0.0f, in.moveZ};

    // Сглаженная скорость (плавный разгон/торможение).
    speed01 += (clamp01(mag) - speed01) * clamp01(dt * 10.0f);

    if (mag > 0.05f) {
        position = position + moveDir * (mag * maxSpeed * dt);
        // Доворот к движению только вперёд/вбок; назад — пятимся.
        if (in.faceMove) {
            float desired = std::atan2(moveDir.x, moveDir.z);
            facingYaw = lerpAngle(facingYaw, desired, clamp01(dt * turnRate));
        }
    }

    // Локомоция: 0 стоим -> 1 идём -> 2 бежим (бег только на сильном отклонении).
    float targetL = (speed01 < 0.02f) ? 0.0f : (1.0f + clamp01((speed01 - 0.5f) / 0.5f));
    animParam += (targetL - animParam) * clamp01(dt * 6.0f);

    animTime += dt;
}

SkinnedItem Character::buildItem(float alpha) const {
    // Интерполяция prev -> current: плавно при рендере чаще тика.
    Vec3 p = prevPosition + (position - prevPosition) * alpha;
    float yaw = lerpAngle(prevFacingYaw, facingYaw, alpha);
    float ap = prevAnimParam + (animParam - prevAnimParam) * alpha;
    float at = prevAnimTime + (animTime - prevAnimTime) * alpha;

    SkinnedItem item;
    item.mesh = mesh;
    item.texture = tex;
    item.color = (tex != 0) ? Vec3{1.0f, 1.0f, 1.0f} : Vec3{0.85f, 0.5f, 0.25f};
    item.model = Mat4::translation(p)
               * Mat4::rotationY(yaw + modelYawOffset)
               * Mat4::scale({scale, scale, scale});

    if (model != nullptr && !model->animations.empty()) {
        int n = (int)model->animations.size();
        auto id = [n](int i) { return i < n ? i : n - 1; };
        if (ap <= 0.01f) {
            model->sampleAnimation(id(0), at, item.joints);
        } else if (ap <= 1.0f) {
            model->sampleBlend(id(0), at, id(1), at, ap, item.joints);
        } else {
            model->sampleBlend(id(1), at, id(2), at, ap - 1.0f, item.joints);
        }
    }
    return item;
}
