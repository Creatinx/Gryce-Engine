#pragma once

#include "physics/physics_world_2d.h"

namespace gryce_engine::physics {

// ---------------------------------------------------------------------------
// BuiltinPhysicsWorld2D — 2D 物理占位后端
// 保留原有自定义 2D 物理系统逻辑；本后端仅作为接口兼容层。
// ---------------------------------------------------------------------------
class BuiltinPhysicsWorld2D : public IPhysicsWorld2D {
public:
    bool init(const math::Vector2f& gravity = math::Vector2f(0.0f, -9.81f)) override;
    void shutdown() override;

    void step(float dt, int velocity_iterations = 8, int position_iterations = 3) override;
    void set_gravity(const math::Vector2f& gravity) override;

    BodyHandle create_body(BodyType type, const math::Vector2f& position, float angle = 0.0f) override;
    void destroy_body(BodyHandle handle) override;
    BodyType get_body_type(BodyHandle handle) const override;

    void set_transform(BodyHandle handle, const math::Vector2f& pos, float angle) override;
    void get_transform(BodyHandle handle, math::Vector2f& out_pos, float& out_angle) const override;

    void set_linear_velocity(BodyHandle handle, const math::Vector2f& vel) override;
    math::Vector2f get_linear_velocity(BodyHandle handle) const override;
    void set_angular_velocity(BodyHandle handle, float vel) override;
    float get_angular_velocity(BodyHandle handle) const override;

    void set_linear_damping(BodyHandle handle, float damping) override;
    void set_angular_damping(BodyHandle handle, float damping) override;
    void set_gravity_scale(BodyHandle handle, float scale) override;

    void apply_force(BodyHandle handle, const math::Vector2f& force, const math::Vector2f& point) override;
    void apply_force_to_center(BodyHandle handle, const math::Vector2f& force) override;
    void apply_impulse(BodyHandle handle, const math::Vector2f& impulse, const math::Vector2f& point) override;

    void wake_up(BodyHandle handle) override;
    bool is_sleeping(BodyHandle handle) const override;
    void set_fixed_rotation(BodyHandle handle, bool fixed) override;

    ShapeHandle add_box_shape(BodyHandle body, const math::Vector2f& half_extents,
                              const math::Vector2f& offset = math::Vector2f::zero(),
                              float angle = 0.0f, const MaterialDesc& material = {}) override;
    ShapeHandle add_circle_shape(BodyHandle body, float radius,
                                 const math::Vector2f& offset = math::Vector2f::zero(),
                                 const MaterialDesc& material = {}) override;
    ShapeHandle add_capsule_shape(BodyHandle body, const math::Vector2f& p1,
                                  const math::Vector2f& p2, float radius,
                                  const MaterialDesc& material = {}) override;
    void destroy_shape(ShapeHandle handle) override;

    std::optional<RaycastHit2D> raycast(const math::Vector2f& origin, const math::Vector2f& direction, float max_distance) const override;

    const char* backend_name() const override { return "Builtin2D"; }
};

} // namespace gryce_engine::physics
