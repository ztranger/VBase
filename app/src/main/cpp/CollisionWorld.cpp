#include "CollisionWorld.h"

// Jolt требует Jolt.h ПЕРВЫМ, до остальных его заголовков.
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <cstdarg>
#include <cstdio>
#include <unordered_map>

#include "Log.h"

JPH_SUPPRESS_WARNINGS

// Псевдоним, чтобы не тащить `using namespace JPH` (иначе JPH::Vec3 конфликтует
// с нашим ::Vec3 из MathUtil). Наш Vec3 остаётся глобальным, Jolt — через J::.
namespace J = JPH;

namespace {

// --- Конвертация наш Vec3 <-> Jolt (одинарная точность: RVec3 == Vec3) ---
inline J::Vec3 toJ(const Vec3& v) { return J::Vec3(v.x, v.y, v.z); }
inline J::RVec3 toJR(const Vec3& v) { return J::RVec3(v.x, v.y, v.z); }
inline Vec3 fromJ(J::Vec3Arg v) { return Vec3{v.GetX(), v.GetY(), v.GetZ()}; }

// --- Слои: статика (NON_MOVING) и подвижные контроллеры (MOVING) ---
namespace Layers {
static constexpr J::ObjectLayer NON_MOVING = 0;
static constexpr J::ObjectLayer MOVING = 1;
static constexpr J::ObjectLayer NUM_LAYERS = 2;
}  // namespace Layers

namespace BP {
static constexpr J::BroadPhaseLayer NON_MOVING(0);
static constexpr J::BroadPhaseLayer MOVING(1);
static constexpr J::uint NUM_LAYERS(2);
}  // namespace BP

class BPLayerInterface final : public J::BroadPhaseLayerInterface {
public:
    BPLayerInterface() {
        map_[Layers::NON_MOVING] = BP::NON_MOVING;
        map_[Layers::MOVING] = BP::MOVING;
    }
    J::uint GetNumBroadPhaseLayers() const override { return BP::NUM_LAYERS; }
    J::BroadPhaseLayer GetBroadPhaseLayer(J::ObjectLayer l) const override { return map_[l]; }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(J::BroadPhaseLayer) const override { return "layer"; }
#endif
private:
    J::BroadPhaseLayer map_[Layers::NUM_LAYERS];
};

class ObjVsBPFilter final : public J::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(J::ObjectLayer l1, J::BroadPhaseLayer l2) const override {
        if (l1 == Layers::NON_MOVING) return l2 == BP::MOVING;
        return true;
    }
};

class ObjVsObjFilter final : public J::ObjectLayerPairFilter {
public:
    bool ShouldCollide(J::ObjectLayer o1, J::ObjectLayer o2) const override {
        if (o1 == Layers::NON_MOVING) return o2 == Layers::MOVING;
        return true;
    }
};

void traceImpl(const char* fmt, ...) {
    va_list list;
    va_start(list, fmt);
    char buf[512];
    std::vsnprintf(buf, sizeof(buf), fmt, list);
    va_end(list);
    LOGI("[Jolt] %s", buf);
}

#ifdef JPH_ENABLE_ASSERTS
bool assertFailedImpl(const char* expr, const char* msg, const char* file, J::uint line) {
    LOGE("[Jolt] %s:%u: (%s) %s", file, (unsigned)line, expr, msg != nullptr ? msg : "");
    return true;
}
#endif

// Глобальная инициализация Jolt — один раз на процесс (аллокатор/фабрика/типы).
// Не разрегистрируем: живёт до конца процесса (несколько CollisionWorld — норма).
void ensureJoltInit() {
    static bool done = false;
    if (done) return;
    done = true;
    J::RegisterDefaultAllocator();
    J::Trace = traceImpl;
    JPH_IF_ENABLE_ASSERTS(J::AssertFailed = assertFailedImpl;)
    J::Factory::sInstance = new J::Factory();
    J::RegisterTypes();
}

}  // namespace

struct CollisionWorld::Impl {
    // Порядок членов важен: интерфейсы объявлены до системы, чтобы разрушались
    // ПОСЛЕ неё (PhysicsSystem держит на них ссылки); контроллеры — после системы,
    // чтобы разрушались ДО неё (CharacterVirtual держит указатель на систему).
    BPLayerInterface bpLayer;
    ObjVsBPFilter objVsBp;
    ObjVsObjFilter objVsObj;

    J::TempAllocatorImpl tempAllocator{4 * 1024 * 1024};
    J::JobSystemSingleThreaded jobSystem{J::cMaxPhysicsJobs};
    J::PhysicsSystem system;

    std::unordered_map<ColliderCharId, J::Ref<J::CharacterVirtual>> characters;
    ColliderCharId nextId = 1;

    Impl() {
        // Немного тел: статика арены + пара контроллеров.
        system.Init(256, 0, 256, 256, bpLayer, objVsBp, objVsObj);
    }
};

CollisionWorld::CollisionWorld() {
    ensureJoltInit();
    impl_ = std::make_unique<Impl>();
}

CollisionWorld::~CollisionWorld() = default;

void CollisionWorld::addBox(Vec3 center, Vec3 half) {
    J::BodyInterface& bodies = impl_->system.GetBodyInterface();
    J::RefConst<J::Shape> shape = new J::BoxShape(toJ(half));
    J::BodyCreationSettings settings(shape, toJR(center), J::Quat::sIdentity(),
                                     J::EMotionType::Static, Layers::NON_MOVING);
    J::Body* body = bodies.CreateBody(settings);
    if (body != nullptr) bodies.AddBody(body->GetID(), J::EActivation::DontActivate);
    else LOGW("CollisionWorld: не удалось создать статичный бокс (лимит тел?)");
}

void CollisionWorld::finalize() { impl_->system.OptimizeBroadPhase(); }

ColliderCharId CollisionWorld::addCharacter(Vec3 pos, float radius, float cylHalfHeight) {
    // Капсула, поднятая на (cylHalfHeight+radius): позиция контроллера = точка ног.
    J::RefConst<J::Shape> capsule = new J::CapsuleShape(cylHalfHeight, radius);
    J::RefConst<J::Shape> shape =
        new J::RotatedTranslatedShape(J::Vec3(0.0f, cylHalfHeight + radius, 0.0f),
                                      J::Quat::sIdentity(), capsule);

    J::CharacterVirtualSettings settings;
    settings.mShape = shape;

    ColliderCharId id = impl_->nextId++;
    impl_->characters[id] =
        new J::CharacterVirtual(&settings, toJR(pos), J::Quat::sIdentity(), &impl_->system);
    return id;
}

void CollisionWorld::removeCharacter(ColliderCharId id) {
    impl_->characters.erase(id);  // Ref<> освобождает CharacterVirtual
}

void CollisionWorld::setCharacterPosition(ColliderCharId id, Vec3 pos) {
    auto it = impl_->characters.find(id);
    if (it == impl_->characters.end()) return;
    J::CharacterVirtual* cv = it->second;
    cv->SetPosition(toJR(pos));
    // Пересчитываем контакты/ground-state на НОВОЙ позиции. Иначе после телепорта
    // (реконсиляция) контроллер держит старое состояние от предсказанной позиции, и
    // первый Update реплея считает движение иначе — прыжок/движение дрожат каждый снапшот.
    cv->RefreshContacts(J::BroadPhaseLayerFilter{}, J::ObjectLayerFilter{},
                        J::BodyFilter{}, J::ShapeFilter{}, impl_->tempAllocator);
}

Vec3 CollisionWorld::characterPosition(ColliderCharId id) const {
    auto it = impl_->characters.find(id);
    if (it == impl_->characters.end()) return Vec3{0.0f, 0.0f, 0.0f};
    return fromJ(it->second->GetPosition());
}

bool CollisionWorld::characterOnGround(ColliderCharId id) const {
    auto it = impl_->characters.find(id);
    if (it == impl_->characters.end()) return false;
    return it->second->GetGroundState() == J::CharacterBase::EGroundState::OnGround;
}

namespace {
constexpr float kGravity = 18.0f;    // ускорение свободного падения (world/с²)
constexpr float kJumpSpeed = 6.0f;   // стартовая скорость прыжка (~1 world высоты)
}  // namespace

Vec3 CollisionWorld::moveCharacter(ColliderCharId id, Vec3 horizontalVelocity, float& velY,
                                   bool jump, float dt) {
    auto it = impl_->characters.find(id);
    if (it == impl_->characters.end()) return Vec3{0.0f, 0.0f, 0.0f};
    J::CharacterVirtual* cv = it->second;

    const J::Vec3 up(0.0f, 1.0f, 0.0f);
    bool onGround = cv->GetGroundState() == J::CharacterBase::EGroundState::OnGround;

    // Вертикальная скорость приходит от владельца (Character.velocityY) — не из
    // контроллера. Так она реконсилируется: сервер шлёт её, клиент ставит перед реплеем,
    // и реплей в точности воспроизводит предсказание (иначе прыжок дёргается онлайн).
    if (onGround && velY <= 0.1f) velY = 0.0f;  // на земле не копим падение
    velY -= kGravity * dt;                      // гравитация каждый тик
    if (jump && onGround) velY = kJumpSpeed;    // прыжок только с земли

    J::Vec3 velocity = toJ(horizontalVelocity) + up * velY;
    cv->SetLinearVelocity(velocity);
    // Фильтры по умолчанию пропускают всё — сталкиваемся со всей статикой мира.
    cv->Update(dt, up * -kGravity, J::BroadPhaseLayerFilter{}, J::ObjectLayerFilter{},
               J::BodyFilter{}, J::ShapeFilter{}, impl_->tempAllocator);
    velY = cv->GetLinearVelocity().Dot(up);  // пост-коллизия (обнулилось о землю/потолок)
    return fromJ(cv->GetPosition());
}
