#pragma once

#include "physics/physics_world_2d.h"

#ifdef GRYCE_HAS_BOX2D

#include <box2d/box2d.h>
#include <unordered_map>
#include <vector>

namespace gryce_engine::physics {

// ---------------------------------------------------------------------------
// Box2DPhysicsWorld2D — Box2D v3 实现的 2D 物理世界
// ---------------------------------------------------------------------------
class Box2DPhysicsWorld2D : public IPhysicsWorld2D {
public:
    Box2DPhysicsWorld2D() = default;
    ~Box2DPhysicsWorld2D() override;

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

    JointHandle create_joint(const JointDesc2D& desc) override;
    void destroy_joint(JointHandle handle) override;

    const char* backend_name() const override { return "Box2D"; }

private:
    b2WorldId world_ = b2_nullWorldId;
    bool initialized_ = false;

    struct BodySlot {
        b2BodyId id = b2_nullBodyId;
        bool used = false;
    };
    struct ShapeSlot {
        b2ShapeId id = b2_nullShapeId;
        bool used = false;
    };
    struct JointSlot {
        b2JointId id = b2_nullJointId;
        bool used = false;
    };

    std::vector<BodySlot> bodies_;
    std::vector<ShapeSlot> shapes_;
    std::vector<JointSlot> joints_;
    std::vector<uint32_t> body_free_list_;
    std::vector<uint32_t> shape_free_list_;
    std::vector<uint32_t> joint_free_list_;

    uint32_t alloc_body_slot();
    uint32_t alloc_shape_slot();
    uint32_t alloc_joint_slot();
    void free_body_slot(uint32_t index);
    void free_shape_slot(uint32_t index);
    void free_joint_slot(uint32_t index);

    b2BodyId get_body_id(BodyHandle handle) const;
    b2ShapeId get_shape_id(ShapeHandle handle) const;
    b2JointId get_joint_id(JointHandle handle) const;
};

} // namespace gryce_engine::physics

#endif // GRYCE_HAS_BOX2D
