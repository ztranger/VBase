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

    speed01 += (clamp01(mag) - speed01) * clamp01(dt * 10.0f);

    if (mag > 0.05f) {
        position = position + moveDir * (mag * maxSpeed * dt);
        if (in.faceMove) {
            float desired = std::atan2(moveDir.x, moveDir.z);
            facingYaw = lerpAngle(facingYaw, desired, clamp01(dt * turnRate));
        }
    }

    // Локомоция: 0 стоим -> 1 идём -> 2 бежим (бег только при сильном отклонении).
    float targetL = (speed01 < 0.02f) ? 0.0f : (1.0f + clamp01((speed01 - 0.5f) / 0.5f));
    animParam += (targetL - animParam) * clamp01(dt * 6.0f);

    animTime += dt;
}
