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
    // Санитизация НЕДОВЕРЕННОГО сетевого ввода. Тот же путь у клиента-предсказания и
    // сервера-авторитета (оба зовут simulate) — значит без расхождения, а честный клиент
    // и так шлёт нормированное направление + magnitude 0..1, для него это no-op.
    //  - не-finite (NaN/Inf) -> ноль (иначе NaN течёт в Jolt, позицию и снапшоты);
    //  - magnitude в [0,1];
    //  - длину направления ограничиваем 1 (|dir|*mag*maxSpeed иначе > maxSpeed = speed hack).
    float mag = std::isfinite(in.magnitude) ? clamp01(in.magnitude) : 0.0f;
    Vec3 moveDir{in.moveX, 0.0f, in.moveZ};
    if (!std::isfinite(moveDir.x) || !std::isfinite(moveDir.z)) {
        moveDir = Vec3{0.0f, 0.0f, 0.0f};
        mag = 0.0f;
    }
    float dirLen2 = moveDir.x * moveDir.x + moveDir.z * moveDir.z;
    if (dirLen2 > 1.0f) {
        float inv = 1.0f / std::sqrt(dirLen2);
        moveDir.x *= inv;
        moveDir.z *= inv;
    }

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
