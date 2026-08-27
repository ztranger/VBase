#pragma once

#include <cstdint>
#include <memory>

#include "engine/core/MathUtil.h"

// Кинематический мир коллизий поверх Jolt Physics. Платформонезависим и НЕ тянет
// Jolt в свой заголовок (pimpl) — как и остальное ядро, за интерфейсом. Держит
// статичную геометрию арены и кинематические контроллеры-капсулы (CharacterVirtual).
//
// Один и тот же мир строится на клиенте (Scene) и на сервере (NetServer) из одного
// описания сцены, чтобы предсказание на клиенте совпадало с авторитетным сервером.

// Хэндл контроллера в мире. 0 — недействительный.
using ColliderCharId = uint32_t;
// Хэндл статичного бокса (футпринт здания). 0 — недействительный.
using ColliderBoxId = uint32_t;

class CollisionWorld {
public:
    CollisionWorld();
    ~CollisionWorld();
    CollisionWorld(const CollisionWorld&) = delete;
    CollisionWorld& operator=(const CollisionWorld&) = delete;

    // --- Статичная геометрия ---
    // Бокс с центром center и полуразмерами half (AABB, без поворота — пока хватает).
    // Можно звать и после finalize (постройка/снос в рантайме). 0 = не удалось создать.
    ColliderBoxId addBox(Vec3 center, Vec3 half);
    void removeBox(ColliderBoxId id);
    // Оптимизировать broad-phase после пачки статики (не обязательно после каждого бокса).
    void finalize();

    // --- Кинематический контроллер-капсула ---
    // pos — точка на земле (ноги). radius/cylHalfHeight — параметры капсулы.
    ColliderCharId addCharacter(Vec3 pos, float radius, float cylHalfHeight);
    // Убрать контроллер (при отключении игрока на сервере).
    void removeCharacter(ColliderCharId id);
    // Телепорт (спавн/реконсиляция) — без коллизий, жёстко ставит позицию.
    void setCharacterPosition(ColliderCharId id, Vec3 pos);
    Vec3 characterPosition(ColliderCharId id) const;
    bool characterOnGround(ColliderCharId id) const;
    // Шаг контроллера: горизонтальная скорость (world) + запрос прыжка за dt.
    // Вертикаль (гравитация/прыжок/земля) ведёт сам мир — вызывать КАЖДЫЙ тик.
    // Возвращает новую позицию ног (уже с учётом столкновений).
    // velY — вертикальная скорость (in/out): владелец (Character) хранит её и
    // реконсилирует, контроллер лишь применяет и возвращает пост-коллизионное значение.
    Vec3 moveCharacter(ColliderCharId id, Vec3 horizontalVelocity, float& velY, bool jump,
                       float dt);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
