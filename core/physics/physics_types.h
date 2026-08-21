#pragma once

#include "math/math.h"
#include <cstdint>
#include <limits>
#include <vector>

namespace gryce_engine::physics {

// 通用句柄类型
using BodyHandle = uint32_t;
using ShapeHandle = uint32_t;
using JointHandle = uint32_t;

// 使用最大值作为无效句柄，避免与合法索引 0 冲突（第一个 shape/body 的索引通常是 0）
constexpr BodyHandle k_invalid_body = std::numeric_limits<BodyHandle>::max();
constexpr ShapeHandle k_invalid_shape = std::numeric_limits<ShapeHandle>::max();
constexpr JointHandle k_invalid_joint = std::numeric_limits<JointHandle>::max();

enum class BodyType {
    Static,
    Kinematic,
    Dynamic
};

enum class ShapeType {
    Box,
    Sphere,
    Capsule,
    Cylinder,
    Plane,
    ConvexHull, // 由模型顶点构建的凸包碰撞体（用于动态刚体）
    Mesh        // 由模型顶点+索引构建的三角网格碰撞体（主要用于静态物体）
};

struct ShapeDesc {
    ShapeType type = ShapeType::Box;
    math::Vector3f size{1.0f, 1.0f, 1.0f}; // box half-extents, sphere radius in x, etc.
    math::Vector3f offset;
    math::Quaternionf rotation;
    float density = 1.0f; // 用于后端根据体积自动计算质量（kg/m^3 相对值）
    std::vector<math::Vector3f> points;     // ConvexHull 顶点 / Mesh 顶点（模型局部空间）
    std::vector<uint32_t> indices;          // Mesh 三角面索引（每 3 个一组）
};

struct BodyDesc {
    BodyType type = BodyType::Dynamic;
    math::Vector3f position;
    math::Quaternionf rotation;
    math::Vector3f linear_velocity;
    math::Vector3f angular_velocity;
    float mass = 1.0f;
    float linear_damping = 0.0f;
    float angular_damping = 0.0f;
    bool allow_sleep = true;
    bool is_sensor = false; // 触发器：产生接触回调但无碰撞响应
    ShapeHandle shape = k_invalid_shape; // 创建 body 时直接附带一个 shape
};

// 碰撞/触发事件类型：BeginContact（开始接触/进入触发器）、EndContact（分离/离开触发器）
enum class CollisionEventType {
    BeginContact,
    EndContact
};

// 由后端 ContactListener 产生的一次接触事件。
// 触发事件（is_trigger=true）无接触点/法线/冲量，point 取两刚体质心中点。
struct CollisionEvent {
    CollisionEventType type = CollisionEventType::BeginContact;
    BodyHandle body_a = k_invalid_body;
    BodyHandle body_b = k_invalid_body;
    math::Vector3f point;
    math::Vector3f normal;
    float impulse = 0.0f; // 仅 BeginContact 时估计的碰撞冲量（沿接触法线）
    bool is_trigger = false;
};

struct MaterialDesc {
    float friction = 0.5f;
    float restitution = 0.2f;
    float density = 1.0f;
    bool is_sensor = false;
};

struct RaycastHit {
    BodyHandle body = k_invalid_body;
    math::Vector3f point;
    math::Vector3f normal;
    float distance = 0.0f;
};

enum class JointType {
    Distance,
    Fixed,
    Hinge,
    Spring,
    Prismatic,
    Wheel,
    Rope
};

struct JointDesc2D {
    JointType type = JointType::Fixed;
    BodyHandle body_a = k_invalid_body;
    BodyHandle body_b = k_invalid_body;
    math::Vector2f anchor_a;
    math::Vector2f anchor_b;
    math::Vector2f axis_a{1.0f, 0.0f};
    math::Vector2f axis_b{1.0f, 0.0f};
    float length = 1.0f;
    float frequency = 0.0f;
    float damping = 0.0f;
    float min_length = 0.0f;
    float max_length = 0.0f;
    bool collide_connected = false;
};

struct JointDesc3D {
    JointType type = JointType::Fixed;
    BodyHandle body_a = k_invalid_body;
    BodyHandle body_b = k_invalid_body;
    math::Vector3f anchor_a;
    math::Vector3f anchor_b;
    math::Vector3f axis_a{0.0f, 0.0f, 1.0f};
    math::Vector3f axis_b{0.0f, 0.0f, 1.0f};
    float length = 1.0f;
    float frequency = 0.0f;
    float damping = 0.0f;
    bool collide_connected = false;
};

} // namespace gryce_engine::physics
