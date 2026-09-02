#include "game/Character.h"

#include <cmath>

#include "engine/physics/CollisionWorld.h"

namespace {
float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
}  // namespace

void Character::snapshot() {
    prevPosition = position;
    prevFacingYaw = facingYaw;
    prevAnimParam = animParam;
    prevAnimTime = animTime;
    prevLocoPhase = locoPhase;
    prevAttackTime = attackTime;
}

void Character::simulate(float dt, const InputCommand& in, CollisionWorld* world) {
    // Старт атаки — ДИСКРЕТНОЕ событие. Ставим таймер здесь (в simulate), чтобы триггер
    // корректно переигрывался при реплее реконсиляции (attackTime всё равно сбрасывается
    // снапшотом перед реплеем). Сам отсчёт таймера — вне simulate (1 раз/тик у владельца),
    // иначе multi-call реплей досрочно завершал бы каст (как и с animTime).
    if (in.attack && attackTime <= 0.0f) attackTime = kAttackDuration;

    // Атака НЕ рутит движение: авто-атака сервера (урон + анимация) не должна конфликтовать
    // с клиентским предсказанием движения — иначе рывки. Персонаж двигается по вводу и во
    // время атаки (кайтинг мага, добивание в движении); attackTime только крутит клип.
    float mag = in.magnitude;
    Vec3 moveDir{in.moveX, 0.0f, in.moveZ};

    speed01 += (clamp01(mag) - speed01) * clamp01(dt * 10.0f);

    Vec3 horizVel{0.0f, 0.0f, 0.0f};  // желаемая горизонтальная скорость
    if (mag > 0.05f) {
        horizVel = moveDir * (mag * maxSpeed);
        if (in.faceMove) {
            float desired = std::atan2(moveDir.x, moveDir.z);
            facingYaw = lerpAngle(facingYaw, desired, clamp01(dt * turnRate));
        }
    }

    if (world != nullptr && collider != 0) {
        // Контроллер прогоняем КАЖДЫЙ тик (гравитация/земля/прыжок), даже стоя на месте.
        // velocityY — наше состояние (реконсилируется), контроллер её только применяет.
        position = world->moveCharacter(collider, horizVel, velocityY, in.jump, dt);
    } else {
        position = position + horizVel * dt;  // fallback без коллизий/гравитации
    }

    // Локомоция: 0 стоим -> 1 идём -> 2 бежим (бег только при сильном отклонении).
    float targetL = (speed01 < 0.02f) ? 0.0f : (1.0f + clamp01((speed01 - 0.5f) / 0.5f));
    animParam += (targetL - animParam) * clamp01(dt * 6.0f);

    // animTime (фаза проигрывания) НЕ трогаем здесь: simulate зовётся несколько раз за
    // тик при реплее реконсиляции, и фаза ускорялась бы. Её крутит владелец 1 раз/тик.
}
