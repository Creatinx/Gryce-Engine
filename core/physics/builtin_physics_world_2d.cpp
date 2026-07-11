#include "physics/builtin_physics_world_2d.h"

#include "utils/glog/glog_lib.h"

namespace gryce_engine::physics {

bool BuiltinPhysicsWorld2D::init(const math::Vector2f& gravity) {
    GLOG_INFO("BuiltinPhysicsWorld2D initialized (gravity={:.2f},{:.2f})", gravity.x, gravity.y);
    return true;
}

void BuiltinPhysicsWorld2D::shutdown() {}

void BuiltinPhysicsWorld2D::set_gravity(const math::Vector2f& gravity) { (void)gravity; }

void BuiltinPhysicsWorld2D::step(float dt, int velocity_iterations, int position_iterations) {
    (void)dt;
    (void)velocity_iterations;
    (void)position_iterations;
}

BodyHandle BuiltinPhysicsWorld2D::create_body(BodyType type, const math::Vector2f& position, float angle) {
    (void)type;
    (void)position;
    (void)angle;
    return k_invalid_body;
}

void BuiltinPhysicsWorld2D::destroy_body(BodyHandle handle) { (void)handle; }

BodyType BuiltinPhysicsWorld2D::get_body_type(BodyHandle handle) const {
    (void)handle;
    return BodyType::Dynamic;
}

void BuiltinPhysicsWorld2D::set_transform(BodyHandle handle, const math::Vector2f& pos, float angle) {
    (void)handle;
    (void)pos;
    (void)angle;
}

void BuiltinPhysicsWorld2D::get_transform(BodyHandle handle, math::Vector2f& out_pos, float& out_angle) const {
    (void)handle;
    out_pos = math::Vector2f::zero();
    out_angle = 0.0f;
}

void BuiltinPhysicsWorld2D::set_linear_velocity(BodyHandle handle, const math::Vector2f& vel) { (void)handle; (void)vel; }
math::Vector2f BuiltinPhysicsWorld2D::get_linear_velocity(BodyHandle handle) const { (void)handle; return math::Vector2f::zero(); }
void BuiltinPhysicsWorld2D::set_angular_velocity(BodyHandle handle, float vel) { (void)handle; (void)vel; }
float BuiltinPhysicsWorld2D::get_angular_velocity(BodyHandle handle) const { (void)handle; return 0.0f; }

void BuiltinPhysicsWorld2D::set_linear_damping(BodyHandle handle, float damping) { (void)handle; (void)damping; }
void BuiltinPhysicsWorld2D::set_angular_damping(BodyHandle handle, float damping) { (void)handle; (void)damping; }
void BuiltinPhysicsWorld2D::set_gravity_scale(BodyHandle handle, float scale) { (void)handle; (void)scale; }

void BuiltinPhysicsWorld2D::apply_force(BodyHandle handle, const math::Vector2f& force, const math::Vector2f& point) {
    (void)handle; (void)force; (void)point;
}
void BuiltinPhysicsWorld2D::apply_force_to_center(BodyHandle handle, const math::Vector2f& force) {
    (void)handle; (void)force;
}
void BuiltinPhysicsWorld2D::apply_impulse(BodyHandle handle, const math::Vector2f& impulse, const math::Vector2f& point) {
    (void)handle; (void)impulse; (void)point;
}

void BuiltinPhysicsWorld2D::wake_up(BodyHandle handle) { (void)handle; }
bool BuiltinPhysicsWorld2D::is_sleeping(BodyHandle handle) const { (void)handle; return false; }
void BuiltinPhysicsWorld2D::set_fixed_rotation(BodyHandle handle, bool fixed) { (void)handle; (void)fixed; }

ShapeHandle BuiltinPhysicsWorld2D::add_box_shape(BodyHandle body, const math::Vector2f& half_extents,
                                                  const math::Vector2f& offset, float angle,
                                                  const MaterialDesc& material) {
    (void)body; (void)half_extents; (void)offset; (void)angle; (void)material;
    return k_invalid_shape;
}

ShapeHandle BuiltinPhysicsWorld2D::add_circle_shape(BodyHandle body, float radius,
                                                     const math::Vector2f& offset,
                                                     const MaterialDesc& material) {
    (void)body; (void)radius; (void)offset; (void)material;
    return k_invalid_shape;
}

ShapeHandle BuiltinPhysicsWorld2D::add_capsule_shape(BodyHandle body, const math::Vector2f& p1,
                                                      const math::Vector2f& p2, float radius,
                                                      const MaterialDesc& material) {
    (void)body; (void)p1; (void)p2; (void)radius; (void)material;
    return k_invalid_shape;
}

void BuiltinPhysicsWorld2D::destroy_shape(ShapeHandle handle) { (void)handle; }

std::optional<IPhysicsWorld2D::RaycastHit2D> BuiltinPhysicsWorld2D::raycast(const math::Vector2f& origin, const math::Vector2f& direction, float max_distance) const {
    (void)origin; (void)direction; (void)max_distance;
    return std::nullopt;
}

} // namespace gryce_engine::physics
