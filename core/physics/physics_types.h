#pragma once

#include "math/math.h"
#include <cstdint>
#include <limits>

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
    Plane
};

struct ShapeDesc {
    ShapeType type = ShapeType::Box;
    math::Vector3f size{1.0f, 1.0f, 1.0f}; // box half-extents, sphere radius in x, etc.
    math::Vector3f offset;
    math::Quaternionf rotation;
    float density = 1.0f; // 用于后端根据体积自动计算质量（kg/m^3 相对值）
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
    ShapeHandle shape = k_invalid_shape; // 创建 body 时直接附带一个 shape
};

struct MaterialDesc {
    float friction = 0.5f;
    float restitution = 0.2f;
    float density = 1.0f;
};

struct RaycastHit {
    BodyHandle body = k_invalid_body;
    math::Vector3f point;
    math::Vector3f normal;
    float distance = 0.0f;
};

} // namespace gryce_engine::physics
