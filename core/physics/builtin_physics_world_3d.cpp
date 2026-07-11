#include "physics/builtin_physics_world_3d.h"

#include "utils/glog/glog_lib.h"

namespace gryce_engine::physics {

bool BuiltinPhysicsWorld3D::init(const math::Vector3f& gravity) {
    GLOG_INFO("BuiltinPhysicsWorld3D initialized (gravity={:.2f},{:.2f},{:.2f})", gravity.x, gravity.y, gravity.z);
    return true;
}

void BuiltinPhysicsWorld3D::shutdown() {}

void BuiltinPhysicsWorld3D::step(float dt, int substeps) {
    (void)dt;
    (void)substeps;
}

BodyHandle BuiltinPhysicsWorld3D::create_body(const BodyDesc& desc) {
    (void)desc;
    return k_invalid_body;
}

void BuiltinPhysicsWorld3D::destroy_body(BodyHandle handle) { (void)handle; }

BodyType BuiltinPhysicsWorld3D::get_body_type(BodyHandle handle) const {
    (void)handle;
    return BodyType::Dynamic;
}

void BuiltinPhysicsWorld3D::set_transform(BodyHandle handle, const math::Vector3f& pos, const math::Quaternionf& rot) {
    (void)handle; (void)pos; (void)rot;
}

void BuiltinPhysicsWorld3D::get_transform(BodyHandle handle, math::Vector3f& out_pos, math::Quaternionf& out_rot) const {
    (void)handle;
    out_pos = math::Vector3f::zero();
    out_rot = math::Quaternionf::identity();
}

void BuiltinPhysicsWorld3D::set_linear_velocity(BodyHandle handle, const math::Vector3f& vel) { (void)handle; (void)vel; }
math::Vector3f BuiltinPhysicsWorld3D::get_linear_velocity(BodyHandle handle) const { (void)handle; return math::Vector3f::zero(); }
void BuiltinPhysicsWorld3D::set_angular_velocity(BodyHandle handle, const math::Vector3f& vel) { (void)handle; (void)vel; }
math::Vector3f BuiltinPhysicsWorld3D::get_angular_velocity(BodyHandle handle) const { (void)handle; return math::Vector3f::zero(); }

void BuiltinPhysicsWorld3D::apply_force(BodyHandle handle, const math::Vector3f& force, const math::Vector3f& point) {
    (void)handle; (void)force; (void)point;
}
void BuiltinPhysicsWorld3D::apply_impulse(BodyHandle handle, const math::Vector3f& impulse, const math::Vector3f& point) {
    (void)handle; (void)impulse; (void)point;
}
void BuiltinPhysicsWorld3D::apply_torque(BodyHandle handle, const math::Vector3f& torque) {
    (void)handle; (void)torque;
}

ShapeHandle BuiltinPhysicsWorld3D::create_shape(const ShapeDesc& desc) {
    (void)desc;
    return k_invalid_shape;
}
void BuiltinPhysicsWorld3D::destroy_shape(ShapeHandle handle) { (void)handle; }
void BuiltinPhysicsWorld3D::attach_shape(BodyHandle body, ShapeHandle shape, const MaterialDesc& material) {
    (void)body; (void)shape; (void)material;
}
void BuiltinPhysicsWorld3D::detach_shape(BodyHandle body, ShapeHandle shape) {
    (void)body; (void)shape;
}

std::optional<RaycastHit> BuiltinPhysicsWorld3D::raycast(const math::Vector3f& origin, const math::Vector3f& direction, float max_distance) const {
    (void)origin; (void)direction; (void)max_distance;
    return std::nullopt;
}

void BuiltinPhysicsWorld3D::foreach_body(std::function<void(BodyHandle, const math::Vector3f&, const math::Quaternionf&)> callback) const {
    (void)callback;
}

} // namespace gryce_engine::physics
