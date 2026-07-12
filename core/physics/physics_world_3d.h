#pragma once

#include "physics/physics_types.h"
#include "math/math.h"
#include <functional>
#include <memory>
#include <optional>

namespace gryce_engine::physics {

// ---------------------------------------------------------------------------
// IPhysicsWorld3D — 3D 物理世界抽象接口
// 底层可由 Jolt、PhysX 或内置物理实现。
// ---------------------------------------------------------------------------
class IPhysicsWorld3D {
public:
    virtual ~IPhysicsWorld3D() = default;

    virtual bool init(const math::Vector3f& gravity = math::Vector3f(0.0f, -9.81f, 0.0f)) = 0;
    virtual void shutdown() = 0;

    virtual void step(float dt, int substeps = 1) = 0;
    virtual void set_gravity(const math::Vector3f& gravity) = 0;

    // 刚体
    virtual BodyHandle create_body(const BodyDesc& desc) = 0;
    virtual void destroy_body(BodyHandle handle) = 0;
    virtual BodyType get_body_type(BodyHandle handle) const = 0;

    virtual void set_transform(BodyHandle handle, const math::Vector3f& pos, const math::Quaternionf& rot) = 0;
    virtual void get_transform(BodyHandle handle, math::Vector3f& out_pos, math::Quaternionf& out_rot) const = 0;

    virtual void set_linear_velocity(BodyHandle handle, const math::Vector3f& vel) = 0;
    virtual math::Vector3f get_linear_velocity(BodyHandle handle) const = 0;
    virtual void set_angular_velocity(BodyHandle handle, const math::Vector3f& vel) = 0;
    virtual math::Vector3f get_angular_velocity(BodyHandle handle) const = 0;

    virtual void set_linear_damping(BodyHandle handle, float damping) = 0;
    virtual void set_angular_damping(BodyHandle handle, float damping) = 0;
    virtual void set_gravity_scale(BodyHandle handle, float scale) = 0;
    virtual void wake_up(BodyHandle handle) = 0;
    virtual bool is_sleeping(BodyHandle handle) const = 0;

    virtual void apply_force(BodyHandle handle, const math::Vector3f& force, const math::Vector3f& point) = 0;
    virtual void apply_impulse(BodyHandle handle, const math::Vector3f& impulse, const math::Vector3f& point) = 0;
    virtual void apply_torque(BodyHandle handle, const math::Vector3f& torque) = 0;

    // 碰撞体
    virtual ShapeHandle create_shape(const ShapeDesc& desc) = 0;
    virtual void destroy_shape(ShapeHandle handle) = 0;
    virtual void attach_shape(BodyHandle body, ShapeHandle shape, const MaterialDesc& material) = 0;
    virtual void detach_shape(BodyHandle body, ShapeHandle shape) = 0;

    // 射线检测
    virtual std::optional<RaycastHit> raycast(const math::Vector3f& origin, const math::Vector3f& direction, float max_distance) const = 0;

    // 查询
    virtual void foreach_body(std::function<void(BodyHandle, const math::Vector3f&, const math::Quaternionf&)> callback) const = 0;

    virtual const char* backend_name() const = 0;
};

// 工厂函数声明（在 physics_factory.cpp 中实现）
std::unique_ptr<IPhysicsWorld3D> create_physics_world_3d(const std::string& backend_name);

} // namespace gryce_engine::physics
