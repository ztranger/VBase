#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "game/Grid.h"
#include "game/SceneTypes.h"  // PendingInput, RemoteEntity, PlayerModel

struct Character;
class CollisionWorld;
struct SceneDesc;
class ClientSession;
struct WorldPresentation;
struct InputCommand;

// P2-18 шаг 4: клиентский МИР — физика (collide-and-slide), приём авторитетных снапшотов +
// реконсиляция предсказания героя, буфер неподтверждённых вводов и футпринт-коллайдеры зданий.
// Владеет «движком» (collision/pending/footprints/simClock/grid); сами сущности (player,
// remoteEntities) и косметику (WorldPresentation) держит Scene и передаёт по ссылке — так рендер/
// пикинг/камера читают сущности из Scene напрямую и остаются нетронутыми этим шагом.
class ClientWorld {
public:
    ClientWorld();
    ~ClientWorld();  // из-за unique_ptr<CollisionWorld> (неполный тип в заголовке)

    // Создать мир коллизий из (санитизированного) описания сцены + посадить кинематический
    // контроллер героя (проставляет player.collider). Сбрасывает pending/футпринты/часы.
    void configure(const SceneDesc& desc, Character& player);
    CollisionWorld* collision() { return collision_.get(); }
    const Grid& grid() const { return grid_; }

    // Записать посланный серверу ввод как неподтверждённый (для реплея) + ограничить окно (P2-03).
    void recordInput(const InputCommand& cmd, float dt);

    // Применить снапшот: реконсиляция героя (player) + обновление/интерполяция remoteEntities +
    // производство боевой косметики в pres. localTeam/localHp/localRespawn — ставки своего героя.
    void applySnapshot(Character& player, std::vector<RemoteEntity>& remotes, ClientSession& session,
                       WorldPresentation& pres, const std::vector<PlayerModel>& mobs,
                       uint8_t& localTeam, float& localHp, float& localRespawn);

    // Синхронизировать футпринт-боксы зданий под текущий список сущностей (для предсказания героя).
    void syncBuildingColliders(const std::vector<RemoteEntity>& remotes);

    void clearPending() { pending_.clear(); }
    double simClock() const { return simClock_; }
    void advanceClock(float dt) { simClock_ += (double)dt; }

private:
    // Yaw «лицом к убийце» моба (ближайший герой/башня — снаряд летит от стрелка). false = нет кандидата.
    bool killerYaw(const std::vector<RemoteEntity>& remotes, const Vec3& heroPos, bool heroActive,
                   const Vec3& mobPos, uint32_t mobId, float& outYaw) const;

    std::unique_ptr<CollisionWorld> collision_;  // кинематическая физика (Jolt)
    std::vector<PendingInput> pending_;           // неподтверждённые вводы (для реплея реконсиляции)
    std::unordered_map<uint32_t, uint32_t> buildingColliders_;  // id здания -> ColliderBoxId
    double simClock_ = 0.0;                        // часы симуляции (сек)
    Grid grid_;                                    // строительная сетка (для футпринтов; копия из сцены)
};
