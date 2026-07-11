#pragma once

#include "math/math.h"
#include <cstdint>

namespace gryce_engine::physics {

// 通用句柄类型
using BodyHandle = uint32_t;
using ShapeHandle = uint32_t;
using JointHandle = uint32_t;

constexpr BodyHandle k_invalid_body = 0;
constexpr ShapeHandle k_invalid_shape = 0;
constexpr JointHandle k_invalid_joint = 0;

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
