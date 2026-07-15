#include "physics/box2d_world_2d.h"

#ifdef GRYCE_HAS_BOX2D

#include "utils/glog/glog_lib.h"
#include <algorithm>
#include <cmath>

namespace gryce_engine::physics {

namespace {

b2BodyType to_box2d(BodyType type) {
    switch (type) {
        case BodyType::Static: return b2_staticBody;
        case BodyType::Kinematic: return b2_kinematicBody;
        case BodyType::Dynamic: return b2_dynamicBody;
    }
    return b2_dynamicBody;
}

BodyType from_box2d(b2BodyType type) {
    switch (type) {
        case b2_staticBody: return BodyType::Static;
        case b2_kinematicBody: return BodyType::Kinematic;
        case b2_dynamicBody: return BodyType::Dynamic;
    }
    return BodyType::Dynamic;
}

math::Vector2f from_b2(b2Vec2 v) {
    return math::Vector2f(v.x, v.y);
}

b2Vec2 to_b2(const math::Vector2f& v) {
    return b2Vec2{v.x, v.y};
}

} // namespace

Box2DPhysicsWorld2D::~Box2DPhysicsWorld2D() {
    shutdown();
}

bool Box2DPhysicsWorld2D::init(const math::Vector2f& gravity) {
    if (initialized_) return true;

    b2WorldDef def = b2DefaultWorldDef();
    def.gravity = to_b2(gravity);
    def.workerCount = 0; // 单线程，避免任务系统依赖

    world_ = b2CreateWorld(&def);
    if (!b2World_IsValid(world_)) {
        GLOG_ERROR("Box2DPhysicsWorld2D: failed to create world");
        return false;
    }

    initialized_ = true;
    GLOG_INFO("Box2DPhysicsWorld2D initialized (gravity={:.2f},{:.2f})", gravity.x, gravity.y);
    return true;
}

void Box2DPhysicsWorld2D::shutdown() {
    if (!initialized_) return;
    b2DestroyWorld(world_);
    world_ = b2_nullWorldId;
    bodies_.clear();
    shapes_.clear();
    joints_.clear();
    body_free_list_.clear();
    shape_free_list_.clear();
    joint_free_list_.clear();
    initialized_ = false;
}

void Box2DPhysicsWorld2D::step(float dt, int velocity_iterations, int position_iterations) {
    (void)position_iterations;
    if (!initialized_) return;
    // Box2D v3 使用子步数（substeps），把 velocity_iterations 当作 substeps 使用
    int substeps = std::max(1, velocity_iterations);
    b2World_Step(world_, dt, substeps);
}

void Box2DPhysicsWorld2D::set_gravity(const math::Vector2f& gravity) {
    if (!initialized_) return;
    b2World_SetGravity(world_, to_b2(gravity));
}

uint32_t Box2DPhysicsWorld2D::alloc_body_slot() {
    if (!body_free_list_.empty()) {
        uint32_t index = body_free_list_.back();
        body_free_list_.pop_back();
        bodies_[index].used = true;
        return index;
    }
    uint32_t index = static_cast<uint32_t>(bodies_.size());
    bodies_.push_back({});
    bodies_[index].used = true;
    return index;
}

uint32_t Box2DPhysicsWorld2D::alloc_shape_slot() {
    if (!shape_free_list_.empty()) {
        uint32_t index = shape_free_list_.back();
        shape_free_list_.pop_back();
        shapes_[index].used = true;
        return index;
    }
    uint32_t index = static_cast<uint32_t>(shapes_.size());
    shapes_.push_back({});
    shapes_[index].used = true;
    return index;
}

uint32_t Box2DPhysicsWorld2D::alloc_joint_slot() {
    if (!joint_free_list_.empty()) {
        uint32_t index = joint_free_list_.back();
        joint_free_list_.pop_back();
        joints_[index].used = true;
        return index;
    }
    uint32_t index = static_cast<uint32_t>(joints_.size());
    joints_.push_back({});
    joints_[index].used = true;
    return index;
}

void Box2DPhysicsWorld2D::free_body_slot(uint32_t index) {
    if (index < bodies_.size()) {
        bodies_[index].used = false;
        bodies_[index].id = b2_nullBodyId;
        body_free_list_.push_back(index);
    }
}

void Box2DPhysicsWorld2D::free_shape_slot(uint32_t index) {
    if (index < shapes_.size()) {
        shapes_[index].used = false;
        shapes_[index].id = b2_nullShapeId;
        shape_free_list_.push_back(index);
    }
}

void Box2DPhysicsWorld2D::free_joint_slot(uint32_t index) {
    if (index < joints_.size()) {
        joints_[index].used = false;
        joints_[index].id = b2_nullJointId;
        joint_free_list_.push_back(index);
    }
}

b2BodyId Box2DPhysicsWorld2D::get_body_id(BodyHandle handle) const {
    if (handle == k_invalid_body) return b2_nullBodyId;
    uint32_t index = handle - 1;
    if (index >= bodies_.size() || !bodies_[index].used) {
        return b2_nullBodyId;
    }
    return bodies_[index].id;
}

b2ShapeId Box2DPhysicsWorld2D::get_shape_id(ShapeHandle handle) const {
    if (handle == k_invalid_shape) return b2_nullShapeId;
    uint32_t index = handle - 1;
    if (index >= shapes_.size() || !shapes_[index].used) {
        return b2_nullShapeId;
    }
    return shapes_[index].id;
}

b2JointId Box2DPhysicsWorld2D::get_joint_id(JointHandle handle) const {
    if (handle == k_invalid_joint) return b2_nullJointId;
    uint32_t index = handle - 1;
    if (index >= joints_.size() || !joints_[index].used) {
        return b2_nullJointId;
    }
    return joints_[index].id;
}

BodyHandle Box2DPhysicsWorld2D::create_body(BodyType type, const math::Vector2f& position, float angle) {
    if (!initialized_) return k_invalid_body;

    b2BodyDef def = b2DefaultBodyDef();
    def.type = to_box2d(type);
    def.position = to_b2(position);
    def.rotation = b2MakeRot(angle);

    b2BodyId id = b2CreateBody(world_, &def);
    if (!b2Body_IsValid(id)) {
        GLOG_ERROR("Box2DPhysicsWorld2D: failed to create body");
        return k_invalid_body;
    }

    uint32_t index = alloc_body_slot();
    bodies_[index].id = id;
    BodyHandle handle = static_cast<BodyHandle>(index + 1);
    b2Body_SetUserData(id, reinterpret_cast<void*>(static_cast<uintptr_t>(handle)));
    return handle;
}

void Box2DPhysicsWorld2D::destroy_body(BodyHandle handle) {
    b2BodyId id = get_body_id(handle);
    if (!b2Body_IsValid(id)) return;
    b2DestroyBody(id);
    if (handle == k_invalid_body) return;
    free_body_slot(handle - 1);
}

BodyType Box2DPhysicsWorld2D::get_body_type(BodyHandle handle) const {
    b2BodyId id = get_body_id(handle);
    if (!b2Body_IsValid(id)) return BodyType::Dynamic;
    return from_box2d(b2Body_GetType(id));
}

void Box2DPhysicsWorld2D::set_transform(BodyHandle handle, const math::Vector2f& pos, float angle) {
    b2BodyId id = get_body_id(handle);
    if (!b2Body_IsValid(id)) return;
    b2Body_SetTransform(id, to_b2(pos), b2MakeRot(angle));
}

void Box2DPhysicsWorld2D::get_transform(BodyHandle handle, math::Vector2f& out_pos, float& out_angle) const {
    b2BodyId id = get_body_id(handle);
    if (!b2Body_IsValid(id)) {
        out_pos = math::Vector2f::zero();
        out_angle = 0.0f;
        return;
    }
    b2Vec2 p = b2Body_GetPosition(id);
    b2Rot r = b2Body_GetRotation(id);
    out_pos = from_b2(p);
    out_angle = std::atan2(r.s, r.c);
}

void Box2DPhysicsWorld2D::set_linear_velocity(BodyHandle handle, const math::Vector2f& vel) {
    b2BodyId id = get_body_id(handle);
    if (!b2Body_IsValid(id)) return;
    b2Body_SetLinearVelocity(id, to_b2(vel));
}

math::Vector2f Box2DPhysicsWorld2D::get_linear_velocity(BodyHandle handle) const {
    b2BodyId id = get_body_id(handle);
    if (!b2Body_IsValid(id)) return math::Vector2f::zero();
    return from_b2(b2Body_GetLinearVelocity(id));
}

void Box2DPhysicsWorld2D::set_angular_velocity(BodyHandle handle, float vel) {
    b2BodyId id = get_body_id(handle);
    if (!b2Body_IsValid(id)) return;
    b2Body_SetAngularVelocity(id, vel);
}

float Box2DPhysicsWorld2D::get_angular_velocity(BodyHandle handle) const {
    b2BodyId id = get_body_id(handle);
    if (!b2Body_IsValid(id)) return 0.0f;
    return b2Body_GetAngularVelocity(id);
}

void Box2DPhysicsWorld2D::set_linear_damping(BodyHandle handle, float damping) {
    b2BodyId id = get_body_id(handle);
    if (!b2Body_IsValid(id)) return;
    b2Body_SetLinearDamping(id, damping);
}

void Box2DPhysicsWorld2D::set_angular_damping(BodyHandle handle, float damping) {
    b2BodyId id = get_body_id(handle);
    if (!b2Body_IsValid(id)) return;
    b2Body_SetAngularDamping(id, damping);
}

void Box2DPhysicsWorld2D::set_gravity_scale(BodyHandle handle, float scale) {
    b2BodyId id = get_body_id(handle);
    if (!b2Body_IsValid(id)) return;
    b2Body_SetGravityScale(id, scale);
}

void Box2DPhysicsWorld2D::apply_force(BodyHandle handle, const math::Vector2f& force, const math::Vector2f& point) {
    b2BodyId id = get_body_id(handle);
    if (!b2Body_IsValid(id)) return;
    b2Body_ApplyForce(id, to_b2(force), to_b2(point), true);
}

void Box2DPhysicsWorld2D::apply_force_to_center(BodyHandle handle, const math::Vector2f& force) {
    b2BodyId id = get_body_id(handle);
    if (!b2Body_IsValid(id)) return;
    b2Body_ApplyForceToCenter(id, to_b2(force), true);
}

void Box2DPhysicsWorld2D::apply_impulse(BodyHandle handle, const math::Vector2f& impulse, const math::Vector2f& point) {
    b2BodyId id = get_body_id(handle);
    if (!b2Body_IsValid(id)) return;
    b2Body_ApplyLinearImpulse(id, to_b2(impulse), to_b2(point), true);
}

void Box2DPhysicsWorld2D::wake_up(BodyHandle handle) {
    b2BodyId id = get_body_id(handle);
    if (!b2Body_IsValid(id)) return;
    b2Body_SetAwake(id, true);
}

bool Box2DPhysicsWorld2D::is_sleeping(BodyHandle handle) const {
    b2BodyId id = get_body_id(handle);
    if (!b2Body_IsValid(id)) return false;
    return !b2Body_IsAwake(id);
}

void Box2DPhysicsWorld2D::set_fixed_rotation(BodyHandle handle, bool fixed) {
    b2BodyId id = get_body_id(handle);
    if (!b2Body_IsValid(id)) return;
    b2Body_SetFixedRotation(id, fixed);
}

ShapeHandle Box2DPhysicsWorld2D::add_box_shape(BodyHandle body, const math::Vector2f& half_extents,
                                                  const math::Vector2f& offset, float angle,
                                                  const MaterialDesc& material) {
    if (!initialized_) return k_invalid_shape;
    b2BodyId body_id = get_body_id(body);
    if (!b2Body_IsValid(body_id)) return k_invalid_shape;

    b2Polygon polygon = b2MakeOffsetBox(half_extents.x, half_extents.y, to_b2(offset), angle);
    b2ShapeDef def = b2DefaultShapeDef();
    def.friction = material.friction;
    def.restitution = material.restitution;
    def.density = material.density;

    b2ShapeId shape_id = b2CreatePolygonShape(body_id, &def, &polygon);
    if (!b2Shape_IsValid(shape_id)) return k_invalid_shape;

    uint32_t index = alloc_shape_slot();
    shapes_[index].id = shape_id;
    return static_cast<ShapeHandle>(index + 1);
}

ShapeHandle Box2DPhysicsWorld2D::add_circle_shape(BodyHandle body, float radius,
                                                   const math::Vector2f& offset,
                                                   const MaterialDesc& material) {
    if (!initialized_) return k_invalid_shape;
    b2BodyId body_id = get_body_id(body);
    if (!b2Body_IsValid(body_id)) return k_invalid_shape;

    b2Circle circle;
    circle.center = to_b2(offset);
    circle.radius = radius;
    b2ShapeDef def = b2DefaultShapeDef();
    def.friction = material.friction;
    def.restitution = material.restitution;
    def.density = material.density;

    b2ShapeId shape_id = b2CreateCircleShape(body_id, &def, &circle);
    if (!b2Shape_IsValid(shape_id)) return k_invalid_shape;

    uint32_t index = alloc_shape_slot();
    shapes_[index].id = shape_id;
    return static_cast<ShapeHandle>(index + 1);
}

ShapeHandle Box2DPhysicsWorld2D::add_capsule_shape(BodyHandle body, const math::Vector2f& p1,
                                                    const math::Vector2f& p2, float radius,
                                                    const MaterialDesc& material) {
    if (!initialized_) return k_invalid_shape;
    b2BodyId body_id = get_body_id(body);
    if (!b2Body_IsValid(body_id)) return k_invalid_shape;

    b2Capsule capsule;
    capsule.center1 = to_b2(p1);
    capsule.center2 = to_b2(p2);
    capsule.radius = radius;
    b2ShapeDef def = b2DefaultShapeDef();
    def.friction = material.friction;
    def.restitution = material.restitution;
    def.density = material.density;

    b2ShapeId shape_id = b2CreateCapsuleShape(body_id, &def, &capsule);
    if (!b2Shape_IsValid(shape_id)) return k_invalid_shape;

    uint32_t index = alloc_shape_slot();
    shapes_[index].id = shape_id;
    return static_cast<ShapeHandle>(index + 1);
}

void Box2DPhysicsWorld2D::destroy_shape(ShapeHandle handle) {
    b2ShapeId id = get_shape_id(handle);
    if (b2Shape_IsValid(id)) {
        b2DestroyShape(id);
    }
    if (handle == k_invalid_shape) return;
    free_shape_slot(handle - 1);
}

std::optional<IPhysicsWorld2D::RaycastHit2D> Box2DPhysicsWorld2D::raycast(const math::Vector2f& origin, const math::Vector2f& direction, float max_distance) const {
    if (!initialized_) return std::nullopt;

    b2Vec2 b2_origin = to_b2(origin);
    b2Vec2 translation = to_b2(direction * max_distance);
    b2QueryFilter filter = b2DefaultQueryFilter();
    b2RayResult result = b2World_CastRayClosest(world_, b2_origin, translation, filter);

    if (!result.hit) return std::nullopt;

    RaycastHit2D hit;
    b2BodyId body_id = b2Shape_GetBody(result.shapeId);
    if (b2Body_IsValid(body_id)) {
        hit.body = static_cast<BodyHandle>(reinterpret_cast<uintptr_t>(b2Body_GetUserData(body_id)));
    } else {
        hit.body = k_invalid_body;
    }
    hit.point = from_b2(result.point);
    hit.normal = from_b2(result.normal);
    hit.fraction = result.fraction;
    return hit;
}

JointHandle Box2DPhysicsWorld2D::create_joint(const JointDesc2D& desc) {
    if (!initialized_) return k_invalid_joint;
    b2BodyId body_a = get_body_id(desc.body_a);
    b2BodyId body_b = get_body_id(desc.body_b);
    if (!b2Body_IsValid(body_a) || !b2Body_IsValid(body_b)) {
        return k_invalid_joint;
    }

    b2JointId joint_id = b2_nullJointId;
    switch (desc.type) {
        case JointType::Distance: {
            b2DistanceJointDef def = b2DefaultDistanceJointDef();
            def.bodyIdA = body_a;
            def.bodyIdB = body_b;
            def.localAnchorA = to_b2(desc.anchor_a);
            def.localAnchorB = to_b2(desc.anchor_b);
            def.length = desc.length;
            def.collideConnected = desc.collide_connected;
            joint_id = b2CreateDistanceJoint(world_, &def);
            break;
        }
        case JointType::Spring: {
            b2DistanceJointDef def = b2DefaultDistanceJointDef();
            def.bodyIdA = body_a;
            def.bodyIdB = body_b;
            def.localAnchorA = to_b2(desc.anchor_a);
            def.localAnchorB = to_b2(desc.anchor_b);
            def.length = desc.length;
            def.enableSpring = true;
            def.hertz = desc.frequency;
            def.dampingRatio = desc.damping;
            def.collideConnected = desc.collide_connected;
            joint_id = b2CreateDistanceJoint(world_, &def);
            break;
        }
        case JointType::Hinge: {
            b2RevoluteJointDef def = b2DefaultRevoluteJointDef();
            def.bodyIdA = body_a;
            def.bodyIdB = body_b;
            def.localAnchorA = to_b2(desc.anchor_a);
            def.localAnchorB = to_b2(desc.anchor_b);
            def.collideConnected = desc.collide_connected;
            joint_id = b2CreateRevoluteJoint(world_, &def);
            break;
        }
        case JointType::Fixed: {
            b2WeldJointDef def = b2DefaultWeldJointDef();
            def.bodyIdA = body_a;
            def.bodyIdB = body_b;
            def.localAnchorA = to_b2(desc.anchor_a);
            def.localAnchorB = to_b2(desc.anchor_b);
            def.collideConnected = desc.collide_connected;
            joint_id = b2CreateWeldJoint(world_, &def);
            break;
        }
    }

    if (!b2Joint_IsValid(joint_id)) return k_invalid_joint;

    uint32_t index = alloc_joint_slot();
    joints_[index].id = joint_id;
    return static_cast<JointHandle>(index + 1);
}

void Box2DPhysicsWorld2D::destroy_joint(JointHandle handle) {
    b2JointId id = get_joint_id(handle);
    if (b2Joint_IsValid(id)) {
        b2DestroyJoint(id);
    }
    if (handle == k_invalid_joint) return;
    free_joint_slot(handle - 1);
}

} // namespace gryce_engine::physics

#endif // GRYCE_HAS_BOX2D
