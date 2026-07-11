#pragma once

#include "physics/physics_world_3d.h"

namespace gryce_engine::physics {

// ---------------------------------------------------------------------------
// BuiltinPhysicsWorld3D — 3D 物理占位后端
// 保留原有自定义 3D 物理系统逻辑；本后端仅作为接口兼容层。
// ---------------------------------------------------------------------------
class BuiltinPhysicsWorld3D : public IPhysicsWorld3D {
public:
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

    const char* backend_name() const override { return "Builtin3D"; }
};

} // namespace gryce_engine::physics
