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
    ShapeHandle add_polygon_shape(BodyHandle body, const std::vector<math::Vector2f>& points,
                                  const math::Vector2f& offset,
                                  const MaterialDesc& material = {}) override;
    ShapeHandle add_segment_shape(BodyHandle body, const math::Vector2f& p1,
                                  const math::Vector2f& p2,
                                  const MaterialDesc& material = {}) override;
    void destroy_shape(ShapeHandle handle) override;

    std::optional<RaycastHit2D> raycast(const math::Vector2f& origin, const math::Vector2f& direction, float max_distance) const override;

    JointHandle create_joint(const JointDesc2D& desc) override;
    void destroy_joint(JointHandle handle) override;

    const char* backend_name() const override { return "Box2D"; }

private:
    // 句柄编码：低 20 位为 slot 索引+1，高 12 位为代际号。
    // slot 被销毁并复用时代际号递增，使“悬垂句柄误触新对象”的 ABA 问题可被检出，
    // 同时保证旧场景文件中 index+1 形式的句柄（代际 0）仍然兼容。
    static constexpr uint32_t k_handle_index_bits = 20;
    static constexpr uint32_t k_handle_index_mask = (1u << k_handle_index_bits) - 1;
    static constexpr uint32_t k_handle_generation_mask = (1u << (32 - k_handle_index_bits)) - 1;

    struct DecodedHandle {
        uint32_t index = 0;       // 0-based slot 索引
        uint32_t generation = 0;  // 代际号
        bool ok = false;
    };

    // BodyHandle/ShapeHandle/JointHandle 均为 uint32_t，编码一致
    static DecodedHandle decode_handle(uint32_t handle) {
        DecodedHandle out;
        if (handle == k_invalid_body) return out;
        const uint32_t low = handle & k_handle_index_mask;
        if (low == 0) return out; // 低 20 位为 0 不是合法编码
        out.index = low - 1;
        out.generation = (handle >> k_handle_index_bits) & k_handle_generation_mask;
        out.ok = true;
        return out;
    }

    static uint32_t encode_handle(uint32_t index, uint32_t generation) {
        return (generation << k_handle_index_bits) | (index + 1);
    }

    b2WorldId world_ = b2_nullWorldId;
    bool initialized_ = false;

    struct BodySlot {
        b2BodyId id = b2_nullBodyId;
        uint32_t generation = 0;
        bool used = false;
    };
    struct ShapeSlot {
        b2ShapeId id = b2_nullShapeId;
        uint32_t generation = 0;
        bool used = false;
    };
    struct JointSlot {
        b2JointId id = b2_nullJointId;
        uint32_t generation = 0;
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
