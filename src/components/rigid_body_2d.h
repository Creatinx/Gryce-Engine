#pragma once

#include "components/component.h"
#include "math/math.h"

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// RigidBody2D — 2D 动态刚体
// ---------------------------------------------------------------------------
class RigidBody2D : public Component {
public:
    float mass = 1.0f;
    bool use_gravity = true;
    bool is_kinematic = false;
    bool fixed_rotation = true; // 2D 平台角色通常不需要旋转

    // 运行时状态
    math::Vector2f velocity = math::Vector2f::zero();
    math::Vector2f acceleration = math::Vector2f::zero();

    // 材质属性
    float restitution = 0.3f; // 弹性（0~1）
    float friction = 0.5f;    // 摩擦（0~1）

    // 线性阻尼
    float linear_damping = 0.01f;

    // 睡眠标记（运行时状态，不序列化）
    bool is_sleeping = false;
    int sleep_frames = 0;

    RigidBody2D() = default;

    const char* type() const override { return "RigidBody2D"; }

    void serialize(nlohmann::json& out) const override {
        out["mass"] = mass;
        out["use_gravity"] = use_gravity;
        out["is_kinematic"] = is_kinematic;
        out["fixed_rotation"] = fixed_rotation;
        out["velocity"] = { velocity.x, velocity.y };
        out["acceleration"] = { acceleration.x, acceleration.y };
        out["restitution"] = restitution;
        out["friction"] = friction;
        out["linear_damping"] = linear_damping;
    }

    void deserialize(const nlohmann::json& in) override {
        mass = in.value("mass", 1.0f);
        use_gravity = in.value("use_gravity", true);
        is_kinematic = in.value("is_kinematic", false);
        fixed_rotation = in.value("fixed_rotation", true);
        auto v = in.value("velocity", std::vector<float>{0, 0});
        if (v.size() >= 2) velocity = math::Vector2f(v[0], v[1]);
        auto a = in.value("acceleration", std::vector<float>{0, 0});
        if (a.size() >= 2) acceleration = math::Vector2f(a[0], a[1]);
        restitution = in.value("restitution", 0.3f);
        friction = in.value("friction", 0.5f);
        linear_damping = in.value("linear_damping", 0.01f);
    }
};

} // namespace gryce_engine::components
