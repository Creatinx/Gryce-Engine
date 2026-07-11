#pragma once

#include "physics/physics_world_3d.h"

#ifdef GRYCE_HAS_JOLT

#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <memory>
#include <vector>
#include <unordered_map>

namespace gryce_engine::physics {

// ---------------------------------------------------------------------------
// JoltPhysicsWorld3D — Jolt Physics v5.x 封装的 3D 物理世界
// ---------------------------------------------------------------------------
class JoltPhysicsWorld3D : public IPhysicsWorld3D {
public:
    JoltPhysicsWorld3D();
    ~JoltPhysicsWorld3D() override;

    bool init(const math::Vector3f& gravity = math::Vector3f(0.0f, -9.81f, 0.0f)) override;
    void shutdown() override;

    void step(float dt, int substeps = 1) override;

    BodyHandle create_body(const BodyDesc& desc) override;
    void destroy_body(BodyHandle handle) override;
    BodyType get_body_type(BodyHandle handle) const override;

    void set_transform(BodyHandle handle, const math::Vector3f& pos, const math::Quaternionf& rot) override;
    void get_transform(BodyHandle handle, math::Vector3f& out_pos, math::Quaternionf& out_rot) const override;

    void set_linear_velocity(BodyHandle handle, const math::Vector3f& vel) override;
    math::Vector3f get_linear_velocity(BodyHandle handle) const override;
    void set_angular_velocity(BodyHandle handle, const math::Vector3f& vel) override;
    math::Vector3f get_angular_velocity(BodyHandle handle) const override;

    void apply_force(BodyHandle handle, const math::Vector3f& force, const math::Vector3f& point) override;
    void apply_impulse(BodyHandle handle, const math::Vector3f& impulse, const math::Vector3f& point) override;
    void apply_torque(BodyHandle handle, const math::Vector3f& torque) override;

    ShapeHandle create_shape(const ShapeDesc& desc) override;
    void destroy_shape(ShapeHandle handle) override;
    void attach_shape(BodyHandle body, ShapeHandle shape, const MaterialDesc& material) override;
    void detach_shape(BodyHandle body, ShapeHandle shape) override;

    std::optional<RaycastHit> raycast(const math::Vector3f& origin, const math::Vector3f& direction, float max_distance) const override;

    void foreach_body(std::function<void(BodyHandle, const math::Vector3f&, const math::Quaternionf&)> callback) const override;

    const char* backend_name() const override { return "Jolt"; }

private:
    bool initialized_ = false;

    // Jolt 子系统
    std::unique_ptr<JPH::TempAllocatorImpl> temp_allocator_;
    std::unique_ptr<JPH::JobSystemThreadPool> job_system_;
    std::unique_ptr<JPH::PhysicsSystem> physics_system_;

    // Body 与 Shape 句柄管理（Jolt BodyID 可直接作为 BodyHandle）
    std::vector<JPH::BodyID> bodies_;
    std::vector<JPH::Ref<JPH::Shape>> shapes_;

    JPH::BodyInterface* body_interface() const;
    JPH::BodyID to_jolt_id(BodyHandle h) const;
    bool is_valid(BodyHandle h) const;
};

} // namespace gryce_engine::physics

#endif // GRYCE_HAS_JOLT
