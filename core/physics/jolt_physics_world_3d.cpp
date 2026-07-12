#include "physics/jolt_physics_world_3d.h"

#ifdef GRYCE_HAS_JOLT

#include <atomic>
#include <mutex>
#include <thread>

#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/PlaneShape.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>

#include "utils/glog/glog_lib.h"

namespace gryce_engine::physics {

namespace {

constexpr JPH::ObjectLayer k_layer_static = 0;
constexpr JPH::ObjectLayer k_layer_dynamic = 1;
constexpr JPH::ObjectLayer k_layers_count = 2;

constexpr JPH::BroadPhaseLayer k_bp_layer_static(0);
constexpr JPH::BroadPhaseLayer k_bp_layer_dynamic(1);
constexpr JPH::BroadPhaseLayer k_bp_layers_count(2);

class BPLayerInterfaceImpl : public JPH::BroadPhaseLayerInterface {
public:
    BPLayerInterfaceImpl() {
        layers_[k_layer_static] = k_bp_layer_static;
        layers_[k_layer_dynamic] = k_bp_layer_dynamic;
    }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const override {
        return layers_[inLayer];
    }
    unsigned GetNumBroadPhaseLayers() const override {
        return k_bp_layers_count.GetValue();
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer inLayer) const override {
        switch (inLayer.GetValue()) {
            case 0: return "Static";
            case 1: return "Dynamic";
            default: return "Invalid";
        }
    }
#endif
private:
    JPH::BroadPhaseLayer layers_[k_layers_count];
};

class ObjectLayerPairFilterImpl : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer, JPH::ObjectLayer) const override { return true; }
};

class ObjectVsBroadPhaseLayerFilterImpl : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer, JPH::BroadPhaseLayer) const override { return true; }
};

static JPH::Vec3 to_jolt(const math::Vector3f& v) {
    return JPH::Vec3(v.x, v.y, v.z);
}

static JPH::Quat to_jolt(const math::Quaternionf& q) {
    return JPH::Quat(q.x, q.y, q.z, q.w);
}

static math::Vector3f from_jolt(const JPH::Vec3& v) {
    return math::Vector3f(v.GetX(), v.GetY(), v.GetZ());
}

static math::Quaternionf from_jolt(const JPH::Quat& q) {
    return math::Quaternionf(q.GetX(), q.GetY(), q.GetZ(), q.GetW());
}

static JPH::EMotionType to_jolt_motion(BodyType t) {
    switch (t) {
        case BodyType::Static:    return JPH::EMotionType::Static;
        case BodyType::Kinematic: return JPH::EMotionType::Kinematic;
        case BodyType::Dynamic:   return JPH::EMotionType::Dynamic;
    }
    return JPH::EMotionType::Dynamic;
}

static BodyType from_jolt_type(JPH::EMotionType t) {
    switch (t) {
        case JPH::EMotionType::Static:    return BodyType::Static;
        case JPH::EMotionType::Kinematic: return BodyType::Kinematic;
        case JPH::EMotionType::Dynamic:   return BodyType::Dynamic;
    }
    return BodyType::Dynamic;
}

std::atomic<int> g_jolt_ref_count{0};
std::once_flag g_jolt_init_flag;

} // namespace

JoltPhysicsWorld3D::JoltPhysicsWorld3D() = default;
JoltPhysicsWorld3D::~JoltPhysicsWorld3D() { shutdown(); }

bool JoltPhysicsWorld3D::init(const math::Vector3f& gravity) {
    if (initialized_) return true;

    std::call_once(g_jolt_init_flag, []() {
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    });
    ++g_jolt_ref_count;

    temp_allocator_ = std::make_unique<JPH::TempAllocatorImpl>(10 * 1024 * 1024);
    job_system_ = std::make_unique<JPH::JobSystemThreadPool>(
        JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
        static_cast<int>(std::thread::hardware_concurrency()) - 1);

    bp_layer_interface_ = new BPLayerInterfaceImpl();
    obj_vs_bp_filter_ = new ObjectVsBroadPhaseLayerFilterImpl();
    obj_pair_filter_ = new ObjectLayerPairFilterImpl();

    physics_system_ = std::make_unique<JPH::PhysicsSystem>();
    physics_system_->Init(
        1024, 0, 1024, 1024,
        *bp_layer_interface_,
        *obj_vs_bp_filter_,
        *obj_pair_filter_);

    physics_system_->SetGravity(to_jolt(gravity));
    initialized_ = true;
    GLOG_INFO("JoltPhysicsWorld3D initialized (ref_count={})", g_jolt_ref_count.load());
    return true;
}

void JoltPhysicsWorld3D::shutdown() {
    if (!initialized_) return;

    if (physics_system_ && !bodies_.empty()) {
        auto* bi = body_interface();
        for (auto& id : bodies_) {
            if (id.IsInvalid()) continue;
            bi->RemoveBody(id);
            bi->DestroyBody(id);
        }
    }
    bodies_.clear();
    shapes_.clear();

    physics_system_.reset();
    job_system_.reset();
    temp_allocator_.reset();

    delete bp_layer_interface_;
    bp_layer_interface_ = nullptr;
    delete obj_vs_bp_filter_;
    obj_vs_bp_filter_ = nullptr;
    delete obj_pair_filter_;
    obj_pair_filter_ = nullptr;

    int prev = --g_jolt_ref_count;
    if (prev == 0) {
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }

    initialized_ = false;
    GLOG_INFO("JoltPhysicsWorld3D shutdown (ref_count={})", g_jolt_ref_count.load());
}

void JoltPhysicsWorld3D::step(float dt, int substeps) {
    if (!initialized_ || !physics_system_) return;
    const int steps = (substeps > 0) ? substeps : 1;
    physics_system_->Update(dt, steps, temp_allocator_.get(), job_system_.get());
}

void JoltPhysicsWorld3D::set_gravity(const math::Vector3f& gravity) {
    if (!initialized_ || !physics_system_) return;
    physics_system_->SetGravity(to_jolt(gravity));
}

JPH::BodyInterface* JoltPhysicsWorld3D::body_interface() const {
    return physics_system_ ? &physics_system_->GetBodyInterface() : nullptr;
}

JPH::BodyID JoltPhysicsWorld3D::to_jolt_id(BodyHandle h) const {
    return JPH::BodyID(h);
}

BodyHandle JoltPhysicsWorld3D::create_body(const BodyDesc& desc) {
    if (!initialized_) return k_invalid_body;
    auto* bi = body_interface();
    if (!bi) return k_invalid_body;

    JPH::BodyCreationSettings settings;
    settings.mPosition = to_jolt(desc.position);
    settings.mRotation = to_jolt(desc.rotation);
    settings.mLinearVelocity = to_jolt(desc.linear_velocity);
    settings.mAngularVelocity = to_jolt(desc.angular_velocity);
    settings.mMotionType = to_jolt_motion(desc.type);
    settings.mObjectLayer = (desc.type == BodyType::Static) ? k_layer_static : k_layer_dynamic;
    settings.mLinearDamping = desc.linear_damping;
    settings.mAngularDamping = desc.angular_damping;
    settings.mAllowSleeping = desc.allow_sleep;

    const JPH::Shape* shape_ptr = nullptr;
    JPH::Ref<JPH::Shape> fallback_shape;
    if (desc.shape != k_invalid_shape && desc.shape < shapes_.size() && shapes_[desc.shape]) {
        shape_ptr = shapes_[desc.shape].GetPtr();
    } else {
        fallback_shape = new JPH::BoxShape(JPH::Vec3(0.5f, 0.5f, 0.5f));
        shape_ptr = fallback_shape.GetPtr();
    }
    settings.SetShape(shape_ptr);

    // 只有当没有提供 shape（或 shape 无有效密度）时才用质量覆盖
    if (desc.shape == k_invalid_shape) {
        settings.mMassPropertiesOverride.mMass = desc.mass;
    }

    JPH::Body* body = bi->CreateBody(settings);
    if (!body) return k_invalid_body;
    JPH::BodyID id = body->GetID();

    bi->AddBody(id, JPH::EActivation::DontActivate);
    if (desc.type == BodyType::Dynamic) {
        bi->ActivateBody(id);
    }

    bodies_.push_back(id);
    return static_cast<BodyHandle>(id.GetIndexAndSequenceNumber());
}

void JoltPhysicsWorld3D::destroy_body(BodyHandle handle) {
    if (!initialized_ || handle == k_invalid_body) return;
    auto id = to_jolt_id(handle);
    auto* bi = body_interface();
    if (!bi || id.IsInvalid()) return;
    bi->RemoveBody(id);
    bi->DestroyBody(id);
    auto it = std::find(bodies_.begin(), bodies_.end(), id);
    if (it != bodies_.end()) bodies_.erase(it);
}

BodyType JoltPhysicsWorld3D::get_body_type(BodyHandle handle) const {
    if (!initialized_ || handle == k_invalid_body) return BodyType::Static;
    auto* bi = body_interface();
    if (!bi) return BodyType::Static;
    return from_jolt_type(bi->GetMotionType(to_jolt_id(handle)));
}

void JoltPhysicsWorld3D::set_transform(BodyHandle handle, const math::Vector3f& pos, const math::Quaternionf& rot) {
    if (!initialized_ || handle == k_invalid_body) return;
    auto* bi = body_interface();
    if (!bi) return;
    bi->SetPositionAndRotation(to_jolt_id(handle), to_jolt(pos), to_jolt(rot), JPH::EActivation::DontActivate);
}

void JoltPhysicsWorld3D::get_transform(BodyHandle handle, math::Vector3f& out_pos, math::Quaternionf& out_rot) const {
    if (!initialized_ || handle == k_invalid_body) return;
    auto* bi = body_interface();
    if (!bi) return;
    auto id = to_jolt_id(handle);
    out_pos = from_jolt(bi->GetPosition(id));
    out_rot = from_jolt(bi->GetRotation(id));
}

void JoltPhysicsWorld3D::set_linear_velocity(BodyHandle handle, const math::Vector3f& vel) {
    if (!initialized_ || handle == k_invalid_body) return;
    auto* bi = body_interface();
    if (!bi) return;
    bi->SetLinearVelocity(to_jolt_id(handle), to_jolt(vel));
}

math::Vector3f JoltPhysicsWorld3D::get_linear_velocity(BodyHandle handle) const {
    if (!initialized_ || handle == k_invalid_body) return math::Vector3f::zero();
    auto* bi = body_interface();
    if (!bi) return math::Vector3f::zero();
    return from_jolt(bi->GetLinearVelocity(to_jolt_id(handle)));
}

void JoltPhysicsWorld3D::set_angular_velocity(BodyHandle handle, const math::Vector3f& vel) {
    if (!initialized_ || handle == k_invalid_body) return;
    auto* bi = body_interface();
    if (!bi) return;
    bi->SetAngularVelocity(to_jolt_id(handle), to_jolt(vel));
}

math::Vector3f JoltPhysicsWorld3D::get_angular_velocity(BodyHandle handle) const {
    if (!initialized_ || handle == k_invalid_body) return math::Vector3f::zero();
    auto* bi = body_interface();
    if (!bi) return math::Vector3f::zero();
    return from_jolt(bi->GetAngularVelocity(to_jolt_id(handle)));
}

void JoltPhysicsWorld3D::set_linear_damping(BodyHandle handle, float damping) {
    if (!initialized_ || handle == k_invalid_body || !physics_system_) return;
    JPH::BodyID id = to_jolt_id(handle);
    JPH::BodyLockWrite lock(physics_system_->GetBodyLockInterface(), id);
    if (lock.Succeeded() && lock.GetBody().IsDynamic()) {
        lock.GetBody().GetMotionProperties()->SetLinearDamping(damping);
    }
}

void JoltPhysicsWorld3D::set_angular_damping(BodyHandle handle, float damping) {
    if (!initialized_ || handle == k_invalid_body || !physics_system_) return;
    JPH::BodyID id = to_jolt_id(handle);
    JPH::BodyLockWrite lock(physics_system_->GetBodyLockInterface(), id);
    if (lock.Succeeded() && lock.GetBody().IsDynamic()) {
        lock.GetBody().GetMotionProperties()->SetAngularDamping(damping);
    }
}

void JoltPhysicsWorld3D::set_gravity_scale(BodyHandle handle, float scale) {
    if (!initialized_ || handle == k_invalid_body) return;
    auto* bi = body_interface();
    if (!bi) return;
    bi->SetGravityFactor(to_jolt_id(handle), scale);
}

void JoltPhysicsWorld3D::wake_up(BodyHandle handle) {
    if (!initialized_ || handle == k_invalid_body) return;
    auto* bi = body_interface();
    if (!bi) return;
    bi->ActivateBody(to_jolt_id(handle));
}

bool JoltPhysicsWorld3D::is_sleeping(BodyHandle handle) const {
    if (!initialized_ || handle == k_invalid_body) return false;
    auto* bi = body_interface();
    if (!bi) return false;
    return !bi->IsActive(to_jolt_id(handle));
}

void JoltPhysicsWorld3D::apply_force(BodyHandle handle, const math::Vector3f& force, const math::Vector3f& point) {
    if (!initialized_ || handle == k_invalid_body) return;
    auto* bi = body_interface();
    if (!bi) return;
    bi->AddForce(to_jolt_id(handle), to_jolt(force), to_jolt(point));
}

void JoltPhysicsWorld3D::apply_impulse(BodyHandle handle, const math::Vector3f& impulse, const math::Vector3f& point) {
    if (!initialized_ || handle == k_invalid_body) return;
    auto* bi = body_interface();
    if (!bi) return;
    bi->AddImpulse(to_jolt_id(handle), to_jolt(impulse), to_jolt(point));
}

void JoltPhysicsWorld3D::apply_torque(BodyHandle handle, const math::Vector3f& torque) {
    if (!initialized_ || handle == k_invalid_body) return;
    auto* bi = body_interface();
    if (!bi) return;
    bi->AddTorque(to_jolt_id(handle), to_jolt(torque));
}

ShapeHandle JoltPhysicsWorld3D::create_shape(const ShapeDesc& desc) {
    if (!initialized_) return k_invalid_shape;

    JPH::Ref<JPH::Shape> shape;
    switch (desc.type) {
        case ShapeType::Box: {
            JPH::BoxShapeSettings settings(to_jolt(desc.size));
            settings.mDensity = desc.density;
            auto result = settings.Create();
            if (result.HasError()) {
                GLOG_ERROR("Jolt: failed to create box shape: {}", result.GetError().c_str());
                return k_invalid_shape;
            }
            shape = result.Get();
            break;
        }
        case ShapeType::Sphere: {
            JPH::SphereShapeSettings settings(desc.size.x);
            settings.mDensity = desc.density;
            auto result = settings.Create();
            if (result.HasError()) {
                GLOG_ERROR("Jolt: failed to create sphere shape: {}", result.GetError().c_str());
                return k_invalid_shape;
            }
            shape = result.Get();
            break;
        }
        case ShapeType::Capsule: {
            JPH::CapsuleShapeSettings settings(desc.size.x, desc.size.y);
            settings.mDensity = desc.density;
            auto result = settings.Create();
            if (result.HasError()) {
                GLOG_ERROR("Jolt: failed to create capsule shape: {}", result.GetError().c_str());
                return k_invalid_shape;
            }
            shape = result.Get();
            break;
        }
        case ShapeType::Plane: {
            // 使用一个很大的盒子近似无限平面，避免 Jolt PlaneShape 的朝向/偏移语义不一致
            shape = new JPH::BoxShape(JPH::Vec3(500.0f, 0.05f, 500.0f));
            break;
        }
    }

    if (!shape) return k_invalid_shape;
    shapes_.push_back(shape);
    return static_cast<ShapeHandle>(shapes_.size() - 1);
}

void JoltPhysicsWorld3D::destroy_shape(ShapeHandle handle) {
    if (handle >= shapes_.size()) return;
    shapes_[handle] = nullptr;
}

void JoltPhysicsWorld3D::attach_shape(BodyHandle body, ShapeHandle shape, const MaterialDesc& material) {
    if (!initialized_ || body == k_invalid_body || shape >= shapes_.size()) return;
    auto* bi = body_interface();
    if (!bi) return;
    auto* s = shapes_[shape].GetPtr();
    if (!s) return;

    auto id = to_jolt_id(body);
    // 只有当 shape 不一致时才替换，避免破坏已经由 create_body 设置的 shape
    if (bi->GetShape(id) != s) {
        bi->SetShape(id, s, false, JPH::EActivation::DontActivate);
    }
    bi->SetFriction(id, material.friction);
    bi->SetRestitution(id, material.restitution);
}

void JoltPhysicsWorld3D::detach_shape(BodyHandle body, ShapeHandle /*shape*/) {
    if (!initialized_ || body == k_invalid_body) return;
    // Jolt body 始终需要 shape；detach 无操作
}

std::optional<RaycastHit> JoltPhysicsWorld3D::raycast(const math::Vector3f& origin, const math::Vector3f& direction, float max_distance) const {
    if (!initialized_ || !physics_system_) return std::nullopt;

    JPH::Vec3 ray_origin = to_jolt(origin);
    JPH::Vec3 ray_dir = to_jolt(direction * max_distance);
    JPH::RRayCast ray(ray_origin, ray_dir);
    JPH::RayCastResult hit;
    if (physics_system_->GetNarrowPhaseQuery().CastRay(ray, hit)) {
        if (hit.mFraction <= 1.0f) {
            RaycastHit result;
            result.body = static_cast<BodyHandle>(hit.mBodyID.GetIndexAndSequenceNumber());
            result.point = from_jolt(ray_origin + hit.mFraction * ray_dir);
            // Jolt 的 RayCastResult 不直接返回法线，这里用零向量占位
            result.normal = math::Vector3f::zero();
            result.distance = hit.mFraction * max_distance;
            return result;
        }
    }
    return std::nullopt;
}

void JoltPhysicsWorld3D::foreach_body(std::function<void(BodyHandle, const math::Vector3f&, const math::Quaternionf&)> callback) const {
    if (!initialized_ || !body_interface()) return;
    for (auto id : bodies_) {
        if (id.IsInvalid()) continue;
        auto* bi = body_interface();
        callback(static_cast<BodyHandle>(id.GetIndexAndSequenceNumber()),
                 from_jolt(bi->GetPosition(id)),
                 from_jolt(bi->GetRotation(id)));
    }
}

} // namespace gryce_engine::physics

#endif // GRYCE_HAS_JOLT
