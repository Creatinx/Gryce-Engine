#pragma once

#include "physics/physics_types.h"
#include "math/math.h"
#include <functional>
#include <memory>
#include <optional>

namespace gryce_engine::physics {

// ---------------------------------------------------------------------------
// IPhysicsWorld2D — 2D 物理世界抽象接口
// 底层可由 Box2D 或内置物理实现。
// ---------------------------------------------------------------------------
class IPhysicsWorld2D {
public:
    virtual ~IPhysicsWorld2D() = default;

    virtual bool init(const math::Vector2f& gravity = math::Vector2f(0.0f, -9.81f)) = 0;
    virtual void shutdown() = 0;

    virtual void step(float dt, int velocity_iterations = 8, int position_iterations = 3) = 0;
    virtual void set_gravity(const math::Vector2f& gravity) = 0;

    // 刚体
    virtual BodyHandle create_body(BodyType type, const math::Vector2f& position, float angle = 0.0f) = 0;
    virtual void destroy_body(BodyHandle handle) = 0;
    virtual BodyType get_body_type(BodyHandle handle) const = 0;

    virtual void set_transform(BodyHandle handle, const math::Vector2f& pos, float angle) = 0;
    virtual void get_transform(BodyHandle handle, math::Vector2f& out_pos, float& out_angle) const = 0;

    virtual void set_linear_velocity(BodyHandle handle, const math::Vector2f& vel) = 0;
    virtual math::Vector2f get_linear_velocity(BodyHandle handle) const = 0;
    virtual void set_angular_velocity(BodyHandle handle, float vel) = 0;
    virtual float get_angular_velocity(BodyHandle handle) const = 0;

    virtual void set_linear_damping(BodyHandle handle, float damping) = 0;
    virtual void set_angular_damping(BodyHandle handle, float damping) = 0;
    virtual void set_gravity_scale(BodyHandle handle, float scale) = 0;

    virtual void apply_force(BodyHandle handle, const math::Vector2f& force, const math::Vector2f& point) = 0;
    virtual void apply_force_to_center(BodyHandle handle, const math::Vector2f& force) = 0;
    virtual void apply_impulse(BodyHandle handle, const math::Vector2f& impulse, const math::Vector2f& point) = 0;

    virtual void wake_up(BodyHandle handle) = 0;
    virtual bool is_sleeping(BodyHandle handle) const = 0;
    virtual void set_fixed_rotation(BodyHandle handle, bool fixed) = 0;

    // 形状：直接附加到 body，返回 shape handle
    virtual ShapeHandle add_box_shape(BodyHandle body, const math::Vector2f& half_extents,
                                      const math::Vector2f& offset = math::Vector2f::zero(),
                                      float angle = 0.0f, const MaterialDesc& material = {}) = 0;
    virtual ShapeHandle add_circle_shape(BodyHandle body, float radius,
                                         const math::Vector2f& offset = math::Vector2f::zero(),
                                         const MaterialDesc& material = {}) = 0;
    virtual ShapeHandle add_capsule_shape(BodyHandle body, const math::Vector2f& p1,
                                          const math::Vector2f& p2, float radius,
                                          const MaterialDesc& material = {}) = 0;
    virtual void destroy_shape(ShapeHandle handle) = 0;

    // 射线检测
    struct RaycastHit2D {
        BodyHandle body = k_invalid_body;
        math::Vector2f point;
        math::Vector2f normal;
        float fraction = 0.0f;
    };
    virtual std::optional<RaycastHit2D> raycast(const math::Vector2f& origin, const math::Vector2f& direction, float max_distance) const = 0;

    virtual const char* backend_name() const = 0;
};

// 工厂函数声明
std::unique_ptr<IPhysicsWorld2D> create_physics_world_2d(const std::string& backend_name);

} // namespace gryce_engine::physics
