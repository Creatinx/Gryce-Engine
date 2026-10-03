#pragma once

#include "components/component.h"
#include "math/math.h"

#include <cstdint>

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// 3D 物理组件族（底层 Jolt Physics，由 PhysicsSystem3D 驱动）
//
// 单位约定：组件与 Transform 使用引擎单位（默认米，与 Jolt 一致，无需换算）。
// 引擎世界为右手系、+Y 向上，与 Jolt 默认一致，故 Transform 位姿可直接下发。
//
// 质量由碰撞体体积 × Jolt 默认密度（1000 kg/m³）自动计算，无需手填。
// ---------------------------------------------------------------------------

// 刚体类型（数值与 JPH::EMotionType 一致，序列化为 int）
enum class Body3DType : int {
    Static = 0,    // 静止，不受力，参与碰撞
    Kinematic = 1, // 运动学，由代码设置速度驱动，不受力
    Dynamic = 2,   // 动力学，受重力/力/碰撞影响
};

// ---------------------------------------------------------------------------
// RigidBody3D — 3D 刚体
// 必须与 BoxCollider3D / SphereCollider3D / CapsuleCollider3D 之一组合；
// 若都没有，系统会补一个 1x1x1 米的默认方块，保证刚体有体积与质量。
// ---------------------------------------------------------------------------
class RigidBody3D : public Component {
public:
    Body3DType body_type = Body3DType::Dynamic;

    math::Vector3f velocity = math::Vector3f(0.0f, 0.0f, 0.0f);        // 引擎单位/秒
    math::Vector3f angular_velocity = math::Vector3f(0.0f, 0.0f, 0.0f); // 弧度/秒
    float linear_damping = 0.05f;
    float angular_damping = 0.05f;
    float gravity_scale = 1.0f;
    float friction = 0.3f;
    float restitution = 0.0f;
    bool allow_sleeping = true;
    bool continuous = false; // 连续碰撞检测（高速子弹）
    bool awake = true;

    // 运行时句柄：Jolt BodyID 打包值（32 位），kNoBody 表示尚未创建（不序列化）。
    // 注意：不能用 0 当哨兵——Jolt 的 BodyID(0) 是合法句柄（无效值是 0xFFFFFFFF），
    // 且首个刚体的打包值可能就是 0，会被误判为"已创建"。
    static constexpr uint64_t kNoBody = 0xFFFFFFFFFFFFFFFFull;
    uint64_t runtime_body = kNoBody;

    RigidBody3D() = default;
    const char* type() const override { return "RigidBody3D"; }

    void serialize(nlohmann::json& out) const override {
        out["body_type"] = static_cast<int>(body_type);
        out["velocity"] = { velocity.x, velocity.y, velocity.z };
        out["angular_velocity"] = { angular_velocity.x, angular_velocity.y, angular_velocity.z };
        out["linear_damping"] = linear_damping;
        out["angular_damping"] = angular_damping;
        out["gravity_scale"] = gravity_scale;
        out["friction"] = friction;
        out["restitution"] = restitution;
        out["allow_sleeping"] = allow_sleeping;
        out["continuous"] = continuous;
        out["awake"] = awake;
    }

    void deserialize(const nlohmann::json& in) override {
        body_type = static_cast<Body3DType>(in.value("body_type", 2));
        auto v = in.value("velocity", std::vector<float>{0.0f, 0.0f, 0.0f});
        if (v.size() >= 3) velocity = math::Vector3f(v[0], v[1], v[2]);
        auto w = in.value("angular_velocity", std::vector<float>{0.0f, 0.0f, 0.0f});
        if (w.size() >= 3) angular_velocity = math::Vector3f(w[0], w[1], w[2]);
        linear_damping = in.value("linear_damping", 0.05f);
        angular_damping = in.value("angular_damping", 0.05f);
        gravity_scale = in.value("gravity_scale", 1.0f);
        friction = in.value("friction", 0.3f);
        restitution = in.value("restitution", 0.0f);
        allow_sleeping = in.value("allow_sleeping", true);
        continuous = in.value("continuous", false);
        awake = in.value("awake", true);
    }

    void snapshot_runtime_state(nlohmann::json& out) const override {
        out["runtime_body"] = runtime_body;
    }
    void restore_runtime_state(const nlohmann::json& in) override {
        runtime_body = in.value("runtime_body", kNoBody);
    }
};

// ---------------------------------------------------------------------------
// BoxCollider3D — 长方体碰撞体
// half_extents 为半长（引擎单位），offset 为相对实体原点的本地偏移。
// ---------------------------------------------------------------------------
class BoxCollider3D : public Component {
public:
    math::Vector3f half_extents = math::Vector3f(0.5f, 0.5f, 0.5f);
    math::Vector3f offset = math::Vector3f(0.0f, 0.0f, 0.0f);
    math::Quaternionf rotation = math::Quaternionf::identity();
    bool is_sensor = false;

    BoxCollider3D() = default;
    const char* type() const override { return "BoxCollider3D"; }

    void serialize(nlohmann::json& out) const override {
        out["half_extents"] = { half_extents.x, half_extents.y, half_extents.z };
        out["offset"] = { offset.x, offset.y, offset.z };
        out["rotation"] = { rotation.x, rotation.y, rotation.z, rotation.w };
        out["is_sensor"] = is_sensor;
    }

    void deserialize(const nlohmann::json& in) override {
        auto h = in.value("half_extents", std::vector<float>{0.5f, 0.5f, 0.5f});
        if (h.size() >= 3) half_extents = math::Vector3f(h[0], h[1], h[2]);
        auto o = in.value("offset", std::vector<float>{0.0f, 0.0f, 0.0f});
        if (o.size() >= 3) offset = math::Vector3f(o[0], o[1], o[2]);
        auto r = in.value("rotation", std::vector<float>{0.0f, 0.0f, 0.0f, 1.0f});
        if (r.size() >= 4) rotation = math::Quaternionf(r[0], r[1], r[2], r[3]);
        is_sensor = in.value("is_sensor", false);
    }
};

// ---------------------------------------------------------------------------
// SphereCollider3D — 球体碰撞体
// ---------------------------------------------------------------------------
class SphereCollider3D : public Component {
public:
    float radius = 0.5f;
    math::Vector3f offset = math::Vector3f(0.0f, 0.0f, 0.0f);
    bool is_sensor = false;

    SphereCollider3D() = default;
    const char* type() const override { return "SphereCollider3D"; }

    void serialize(nlohmann::json& out) const override {
        out["radius"] = radius;
        out["offset"] = { offset.x, offset.y, offset.z };
        out["is_sensor"] = is_sensor;
    }

    void deserialize(const nlohmann::json& in) override {
        radius = in.value("radius", 0.5f);
        auto o = in.value("offset", std::vector<float>{0.0f, 0.0f, 0.0f});
        if (o.size() >= 3) offset = math::Vector3f(o[0], o[1], o[2]);
        is_sensor = in.value("is_sensor", false);
    }
};

// ---------------------------------------------------------------------------
// CapsuleCollider3D — 胶囊碰撞体（角色常用）
// half_height 为圆柱段半高（不含两端半球），总高 = 2 * (half_height + radius)。
// ---------------------------------------------------------------------------
class CapsuleCollider3D : public Component {
public:
    float radius = 0.3f;
    float half_height = 0.6f;
    math::Vector3f offset = math::Vector3f(0.0f, 0.0f, 0.0f);
    math::Quaternionf rotation = math::Quaternionf::identity();
    bool is_sensor = false;

    CapsuleCollider3D() = default;
    const char* type() const override { return "CapsuleCollider3D"; }

    void serialize(nlohmann::json& out) const override {
        out["radius"] = radius;
        out["half_height"] = half_height;
        out["offset"] = { offset.x, offset.y, offset.z };
        out["rotation"] = { rotation.x, rotation.y, rotation.z, rotation.w };
        out["is_sensor"] = is_sensor;
    }

    void deserialize(const nlohmann::json& in) override {
        radius = in.value("radius", 0.3f);
        half_height = in.value("half_height", 0.6f);
        auto o = in.value("offset", std::vector<float>{0.0f, 0.0f, 0.0f});
        if (o.size() >= 3) offset = math::Vector3f(o[0], o[1], o[2]);
        auto r = in.value("rotation", std::vector<float>{0.0f, 0.0f, 0.0f, 1.0f});
        if (r.size() >= 4) rotation = math::Quaternionf(r[0], r[1], r[2], r[3]);
        is_sensor = in.value("is_sensor", false);
    }
};

} // namespace gryce_engine::components
