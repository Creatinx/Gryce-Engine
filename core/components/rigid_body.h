#pragma once

#include "components/component.h"
#include "math/math.h"

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// RigidBody — 动态刚体占位组件。
// ---------------------------------------------------------------------------
class RigidBody : public Component {
public:
    float mass = 1.0f;
    bool use_gravity = true;
    bool is_kinematic = false;

    // 运行时状态（也序列化，便于保存初始速度）
    math::Vector3f velocity = math::Vector3f::zero();
    math::Vector3f acceleration = math::Vector3f::zero();

    // 角运动占位字段（供未来扩展真正的角物理）
    math::Vector3f angular_velocity = math::Vector3f::zero();
    math::Matrix4f inertia_tensor = math::Matrix4f::identity();

    // 材质属性
    float restitution = 0.3f; // 弹性（0~1）
    float friction = 0.5f;    // 摩擦（0~1）

    // 线性阻尼与角阻尼（占位）
    float linear_damping = 0.0f;
    float angular_damping = 0.0f;

    // 睡眠标记（运行时状态，不序列化）：速度持续很小时跳过积分
    bool is_sleeping = false;
    int sleep_frames = 0;

    // 上一帧最大碰撞冲量（运行时状态，不序列化；供 DestructibleBody 等使用）
    float last_collision_impulse = 0.0f;

    RigidBody() = default;

    const char* type() const override { return "RigidBody"; }

    void serialize(nlohmann::json& out) const override {
        out["mass"] = mass;
        out["use_gravity"] = use_gravity;
        out["is_kinematic"] = is_kinematic;
        out["velocity"] = { velocity.x, velocity.y, velocity.z };
        out["acceleration"] = { acceleration.x, acceleration.y, acceleration.z };
        out["angular_velocity"] = { angular_velocity.x, angular_velocity.y, angular_velocity.z };
        out["inertia_tensor"] = {
            inertia_tensor(0, 0), inertia_tensor(1, 0), inertia_tensor(2, 0), inertia_tensor(3, 0),
            inertia_tensor(0, 1), inertia_tensor(1, 1), inertia_tensor(2, 1), inertia_tensor(3, 1),
            inertia_tensor(0, 2), inertia_tensor(1, 2), inertia_tensor(2, 2), inertia_tensor(3, 2),
            inertia_tensor(0, 3), inertia_tensor(1, 3), inertia_tensor(2, 3), inertia_tensor(3, 3)
        };
        out["restitution"] = restitution;
        out["friction"] = friction;
        out["linear_damping"] = linear_damping;
        out["angular_damping"] = angular_damping;
    }

    void deserialize(const nlohmann::json& in) override {
        mass = in.value("mass", 1.0f);
        use_gravity = in.value("use_gravity", true);
        is_kinematic = in.value("is_kinematic", false);
        auto v = in.value("velocity", std::vector<float>{0, 0, 0});
        if (v.size() >= 3) velocity = math::Vector3f(v[0], v[1], v[2]);
        auto a = in.value("acceleration", std::vector<float>{0, 0, 0});
        if (a.size() >= 3) acceleration = math::Vector3f(a[0], a[1], a[2]);
        auto av = in.value("angular_velocity", std::vector<float>{0, 0, 0});
        if (av.size() >= 3) angular_velocity = math::Vector3f(av[0], av[1], av[2]);
        auto it = in.value("inertia_tensor", std::vector<float>{
            1, 0, 0, 0,
            0, 1, 0, 0,
            0, 0, 1, 0,
            0, 0, 0, 1
        });
        if (it.size() >= 16) {
            inertia_tensor(0, 0) = it[0];  inertia_tensor(1, 0) = it[1];  inertia_tensor(2, 0) = it[2];  inertia_tensor(3, 0) = it[3];
            inertia_tensor(0, 1) = it[4];  inertia_tensor(1, 1) = it[5];  inertia_tensor(2, 1) = it[6];  inertia_tensor(3, 1) = it[7];
            inertia_tensor(0, 2) = it[8];  inertia_tensor(1, 2) = it[9];  inertia_tensor(2, 2) = it[10]; inertia_tensor(3, 2) = it[11];
            inertia_tensor(0, 3) = it[12]; inertia_tensor(1, 3) = it[13]; inertia_tensor(2, 3) = it[14]; inertia_tensor(3, 3) = it[15];
        }
        restitution = in.value("restitution", 0.3f);
        friction = in.value("friction", 0.5f);
        linear_damping = in.value("linear_damping", 0.0f);
        angular_damping = in.value("angular_damping", 0.0f);
    }
};

} // namespace gryce_engine::components
