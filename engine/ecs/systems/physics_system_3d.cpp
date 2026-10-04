#include "ecs/systems/physics_system_3d.h"

// Jolt 要求：Jolt/Jolt.h 必须是第一个被包含的 Jolt 头文件。
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/Memory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include "components/physics_3d.h"
#include "components/transform.h"
#include "scene/entity.h"
#include "scene/query.h"
#include "scene/scene.h"
#include "utils/glog/glog_lib.h"

#include <algorithm>
#include <cmath>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace gryce_engine::ecs {

namespace {

// ---------------------------------------------------------------------------
// 碰撞分层：静态物体与动态物体分属不同层，静态之间不互相检测。
// ---------------------------------------------------------------------------
namespace Layers {
constexpr JPH::ObjectLayer NON_MOVING = 0;
constexpr JPH::ObjectLayer MOVING = 1;
constexpr JPH::ObjectLayer NUM_LAYERS = 2;
} // namespace Layers

namespace BroadPhaseLayers {
constexpr JPH::BroadPhaseLayer NON_MOVING(0);
constexpr JPH::BroadPhaseLayer MOVING(1);
constexpr JPH::uint NUM_LAYERS = 2;
} // namespace BroadPhaseLayers

class BPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::NUM_LAYERS; }

    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const override {
        switch (inLayer) {
            case Layers::NON_MOVING: return BroadPhaseLayers::NON_MOVING;
            case Layers::MOVING:     return BroadPhaseLayers::MOVING;
            default:                 return BroadPhaseLayers::MOVING;
        }
    }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer inLayer) const override {
        switch (static_cast<JPH::BroadPhaseLayer::Type>(inLayer)) {
            case static_cast<JPH::BroadPhaseLayer::Type>(BroadPhaseLayers::NON_MOVING): return "NON_MOVING";
            case static_cast<JPH::BroadPhaseLayer::Type>(BroadPhaseLayers::MOVING):     return "MOVING";
            default:                                                                    return "INVALID";
        }
    }
#endif
};

class ObjectVsBroadPhaseLayerFilterImpl final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer inLayer1, JPH::BroadPhaseLayer inLayer2) const override {
        switch (inLayer1) {
            case Layers::NON_MOVING: return inLayer2 == BroadPhaseLayers::MOVING;
            case Layers::MOVING:     return true;
            default:                 return false;
        }
    }
};

class ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer inObject1, JPH::ObjectLayer inObject2) const override {
        switch (inObject1) {
            case Layers::NON_MOVING: return inObject2 == Layers::MOVING;
            case Layers::MOVING:     return true;
            default:                 return false;
        }
    }
};

// Jolt 全局初始化（分配器 / 工厂 / 类型注册）只允许执行一次。
// 进程生命周期内不反注册，避免多个物理系统实例重复初始化导致崩溃。
void ensure_jolt_globals() {
    static bool done = false;
    if (done) return;
    done = true;
    JPH::RegisterDefaultAllocator();
    if (JPH::Factory::sInstance == nullptr) {
        JPH::Factory::sInstance = new JPH::Factory();
    }
    JPH::RegisterTypes();
}

inline JPH::Vec3 to_jolt(const math::Vector3f& v) { return JPH::Vec3(v.x, v.y, v.z); }
inline JPH::RVec3 to_jolt_r(const math::Vector3f& v) { return JPH::RVec3(v.x, v.y, v.z); }
inline JPH::Quat to_jolt(const math::Quaternionf& q) { return JPH::Quat(q.x, q.y, q.z, q.w); }

// Jolt 未启用双精度时 RVec3 与 Vec3 是同一类型，故用模板统一处理两者。
template<typename V>
inline math::Vector3f to_v3(const V& v) {
    return math::Vector3f(static_cast<float>(v.GetX()),
                          static_cast<float>(v.GetY()),
                          static_cast<float>(v.GetZ()));
}
inline math::Quaternionf to_quat(const JPH::Quat& q) {
    return math::Quaternionf(q.GetX(), q.GetY(), q.GetZ(), q.GetW());
}

inline JPH::BodyID unpack_body(uint64_t v) { return JPH::BodyID(static_cast<JPH::uint32>(v)); }
inline uint64_t pack_body(const JPH::BodyID& id) { return id.GetIndexAndSequenceNumber(); }

JPH::ObjectLayer layer_for(components::Body3DType t) {
    return t == components::Body3DType::Static ? Layers::NON_MOVING : Layers::MOVING;
}

JPH::EMotionType motion_for(components::Body3DType t) {
    return static_cast<JPH::EMotionType>(static_cast<int>(t));
}

} // namespace

struct PhysicsSystem3D::Impl {
    // 注意：必须是可重建的指针，不能是值成员。JPH::PhysicsSystem 只有 Init()、
    // 没有 Shutdown()，在同一个对象上二次 Init() 会踩到
    // BodyManager -> MutexArray::Init 里的 JPH_ASSERT(mMutexStorage == nullptr)，
    // 直接 __debugbreak() 终止进程。场景切换（attach_scene -> shutdown -> init）
    // 正好会二次 init，所以每次 init 都要整体销毁再新建。
    std::unique_ptr<JPH::PhysicsSystem> physics;
    std::unique_ptr<JPH::TempAllocatorImpl> temp_allocator;
    std::unique_ptr<JPH::JobSystemThreadPool> job_system;
    BPLayerInterfaceImpl bp_layer_interface;
    ObjectVsBroadPhaseLayerFilterImpl object_vs_bp_filter;
    ObjectLayerPairFilterImpl object_layer_pair_filter;

    std::unordered_map<EntityID, JPH::BodyID> bodies;
    math::Vector3f gravity = math::Vector3f(0.0f, -9.81f, 0.0f);
    float accumulator = 0.0f;
    bool initialized = false;
};

PhysicsSystem3D::PhysicsSystem3D() : impl_(std::make_unique<Impl>()) {}
PhysicsSystem3D::~PhysicsSystem3D() = default;

void PhysicsSystem3D::set_gravity(const math::Vector3f& g) {
    impl_->gravity = g;
    if (impl_->initialized && impl_->physics) {
        impl_->physics->SetGravity(to_jolt(g));
    }
}

math::Vector3f PhysicsSystem3D::get_gravity3() const { return impl_->gravity; }

void PhysicsSystem3D::on_init(scene::Scene& scene) {
    (void)scene;
    ensure_jolt_globals();

    impl_->bodies.clear();
    impl_->accumulator = 0.0f;

    // 销毁上一次的 PhysicsSystem（连同 BodyManager / 宽相等的内部状态），
    // 否则二次 Init() 会触发 Jolt 内部断言。见 Impl 中 physics 的注释。
    impl_->physics.reset();
    impl_->physics = std::make_unique<JPH::PhysicsSystem>();

    constexpr JPH::uint cMaxPhysicsJobs = 2048;
    constexpr JPH::uint cMaxPhysicsBarriers = 8;
    const size_t temp_size = 16 * 1024 * 1024;
    impl_->temp_allocator = std::make_unique<JPH::TempAllocatorImpl>(temp_size);

    const int threads = std::max(0, worker_threads);
    impl_->job_system = std::make_unique<JPH::JobSystemThreadPool>(
        cMaxPhysicsJobs, cMaxPhysicsBarriers, threads);

    impl_->physics->Init(
        static_cast<JPH::uint>(std::max(1, max_bodies)),
        0,
        static_cast<JPH::uint>(std::max(1, max_body_pairs)),
        static_cast<JPH::uint>(std::max(1, max_contact_constraints)),
        impl_->bp_layer_interface,
        impl_->object_vs_bp_filter,
        impl_->object_layer_pair_filter);

    impl_->physics->SetGravity(to_jolt(impl_->gravity));
    impl_->initialized = true;

    GLOG_INFO("PhysicsSystem3D: Jolt initialized (bodies={}, pairs={}, threads={})",
              max_bodies, max_body_pairs, threads);
}

void PhysicsSystem3D::on_shutdown(scene::Scene& scene) {
    (void)scene;
    if (!impl_->initialized) return;

    JPH::BodyInterface& bi = impl_->physics->GetBodyInterface();
    for (auto& [entity, id] : impl_->bodies) {
        if (!id.IsInvalid()) bi.DestroyBody(id);
    }
    impl_->bodies.clear();

    impl_->job_system.reset();
    impl_->temp_allocator.reset();
    // 真正销毁 PhysicsSystem，让下一次 on_init 能干净地重新 Init()
    impl_->physics.reset();
    impl_->initialized = false;
    impl_->accumulator = 0.0f;
}

void PhysicsSystem3D::on_update(scene::Scene& scene, float dt) {
    if (!impl_->initialized) return;

    JPH::BodyInterface& bi = impl_->physics->GetBodyInterface();

    // --- 1. 生命周期同步：创建缺失刚体、销毁已失效刚体 -------------------
    std::unordered_set<EntityID> live;
    auto rb_pool = scene.component_store().pool<components::RigidBody3D>();
    live.reserve(rb_pool.size());

    for (components::RigidBody3D* rb : rb_pool) {
        if (!rb) continue;
        scene::Entity* e = rb->owner();
        if (!e) continue;
        live.insert(e->id());

        const JPH::BodyID existing = unpack_body(rb->runtime_body);
        if (rb->runtime_body != components::RigidBody3D::kNoBody && !existing.IsInvalid()) {
            continue; // 已创建；类型变更经组件重建（移除再添加）触发
        }

        // 碰撞体：Box > Sphere > Capsule；都没有则补 1x1x1 米默认方块。
        JPH::RefConst<JPH::Shape> shape;
        bool is_sensor = false;
        if (auto* box = e->get_component<components::BoxCollider3D>()) {
            const JPH::Vec3 he(std::max(0.001f, box->half_extents.x),
                               std::max(0.001f, box->half_extents.y),
                               std::max(0.001f, box->half_extents.z));
            shape = new JPH::BoxShape(he);
            is_sensor = box->is_sensor;
        } else if (auto* sphere = e->get_component<components::SphereCollider3D>()) {
            shape = new JPH::SphereShape(std::max(0.001f, sphere->radius));
            is_sensor = sphere->is_sensor;
        } else if (auto* capsule = e->get_component<components::CapsuleCollider3D>()) {
            shape = new JPH::CapsuleShape(std::max(0.001f, capsule->half_height),
                                          std::max(0.001f, capsule->radius));
            is_sensor = capsule->is_sensor;
        } else {
            shape = new JPH::BoxShape(JPH::Vec3(0.5f, 0.5f, 0.5f));
        }

        const auto* tr = e->transform();
        const math::Vector3f pos = tr ? tr->position : math::Vector3f::zero();
        const math::Quaternionf rot = tr ? tr->rotation : math::Quaternionf::identity();

        JPH::BodyCreationSettings bcs(
            shape, to_jolt_r(pos), to_jolt(rot),
            motion_for(rb->body_type), layer_for(rb->body_type));
        bcs.mLinearVelocity = to_jolt(rb->velocity);
        bcs.mAngularVelocity = to_jolt(rb->angular_velocity);
        bcs.mLinearDamping = rb->linear_damping;
        bcs.mAngularDamping = rb->angular_damping;
        bcs.mGravityFactor = rb->gravity_scale;
        bcs.mAllowSleeping = rb->allow_sleeping;
        bcs.mFriction = rb->friction;
        bcs.mRestitution = rb->restitution;
        bcs.mIsSensor = is_sensor;
        bcs.mUserData = static_cast<JPH::uint64>(e->id());
        if (rb->continuous) {
            bcs.mMotionQuality = JPH::EMotionQuality::LinearCast;
        }
        if (!rb->awake) {
            bcs.mAllowSleeping = true;
        }

        const JPH::BodyID id = bi.CreateAndAddBody(bcs, JPH::EActivation::Activate);
        if (id.IsInvalid()) {
            GLOG_WARN("PhysicsSystem3D: failed to create body for '{}'", e->name());
            rb->runtime_body = components::RigidBody3D::kNoBody;
            continue;
        }
        impl_->bodies[e->id()] = id;
        rb->runtime_body = pack_body(id);
    }

    for (auto it = impl_->bodies.begin(); it != impl_->bodies.end();) {
        if (!live.contains(it->first)) {
            if (!it->second.IsInvalid()) bi.DestroyBody(it->second);
            it = impl_->bodies.erase(it);
        } else {
            ++it;
        }
    }

    // --- 2. 固定步长推进 --------------------------------------------------
    impl_->accumulator += std::min(dt, 0.25f);
    constexpr float kStep = 1.0f / 60.0f;
    const int max_steps = std::max(1, max_steps_per_frame);
    int steps = 0;
    while (impl_->accumulator >= kStep && steps < max_steps) {
        // 步进前把静态/运动学刚体跟随 Transform，动力学下发脚本速度
        for (components::RigidBody3D* rb : rb_pool) {
            if (!rb || !rb->enabled) continue;
            scene::Entity* e = rb->owner();
            if (!e || !e->enabled) continue;
            if (rb->runtime_body == components::RigidBody3D::kNoBody) continue;
            const JPH::BodyID id = unpack_body(rb->runtime_body);
            if (id.IsInvalid()) continue;

            if (rb->body_type == components::Body3DType::Dynamic) {
                // 只在组件速度被外部（脚本）改写时才下发：回写阶段会把物理
                // 结果同步回组件，故未被改动的值必然与刚体当前速度一致。
                // 若无条件下发，会覆盖重力积分与碰撞求解的结果（多子步时尤甚）。
                const math::Vector3f bv = to_v3(bi.GetLinearVelocity(id));
                const math::Vector3f& want = rb->velocity;
                if (std::fabs(want.x - bv.x) > 1e-3f || std::fabs(want.y - bv.y) > 1e-3f ||
                    std::fabs(want.z - bv.z) > 1e-3f) {
                    bi.SetLinearVelocity(id, to_jolt(want));
                }
                const math::Vector3f bw = to_v3(bi.GetAngularVelocity(id));
                const math::Vector3f& wwant = rb->angular_velocity;
                if (std::fabs(wwant.x - bw.x) > 1e-3f || std::fabs(wwant.y - bw.y) > 1e-3f ||
                    std::fabs(wwant.z - bw.z) > 1e-3f) {
                    bi.SetAngularVelocity(id, to_jolt(wwant));
                }
            } else {
                const auto* tr = e->transform();
                const math::Vector3f pos = tr ? tr->position : math::Vector3f::zero();
                const math::Quaternionf rot = tr ? tr->rotation : math::Quaternionf::identity();
                bi.SetPositionAndRotation(id, to_jolt_r(pos), to_jolt(rot),
                                          JPH::EActivation::Activate);
                if (rb->body_type == components::Body3DType::Kinematic) {
                    bi.SetLinearVelocity(id, to_jolt(rb->velocity));
                    bi.SetAngularVelocity(id, to_jolt(rb->angular_velocity));
                }
            }
        }

        impl_->physics->Update(kStep, 1, impl_->temp_allocator.get(), impl_->job_system.get());
        impl_->accumulator -= kStep;
        ++steps;
    }
    if (steps >= max_steps) {
        impl_->accumulator = 0.0f; // 丢弃积压，避免卡顿后死亡螺旋
    }

    // --- 3. 回写：位姿 -> Transform，速度 -> RigidBody3D ------------------
    for (components::RigidBody3D* rb : rb_pool) {
        if (!rb) continue;
        scene::Entity* e = rb->owner();
        if (!e) continue;
        if (rb->runtime_body == components::RigidBody3D::kNoBody) continue;
        const JPH::BodyID id = unpack_body(rb->runtime_body);
        if (id.IsInvalid()) continue;

        if (auto* tr = e->transform()) {
            tr->position = to_v3(bi.GetPosition(id));
            tr->rotation = to_quat(bi.GetRotation(id));
        }
        rb->velocity = to_v3(bi.GetLinearVelocity(id));
        rb->angular_velocity = to_v3(bi.GetAngularVelocity(id));
        rb->awake = bi.IsActive(id);
    }
}

} // namespace gryce_engine::ecs
