#pragma once

#include "components/component.h"
#include "math/math.h"

#include <cstdint>

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// 2D 物理组件族（底层 Box2D v3 C API，由 PhysicsSystem2D 驱动）
//
// 单位约定：组件与 Transform 使用引擎单位（默认像素），PhysicsSystem2D 按
// pixels_per_meter 换算到 Box2D 的米制世界。力的输入一律以"加速度"表达
// （引擎单位/秒²），由系统乘以刚体质量换算为牛顿力，避免不同质量物体受
// 风力/水流影响不一致。
// ---------------------------------------------------------------------------

// 刚体类型（与 b2BodyType 数值一致，序列化为 int）
enum class Body2DType : int {
    Static = 0,    // 静止，不受力，参与碰撞
    Kinematic = 1, // 运动学，由代码设置速度驱动，不受力
    Dynamic = 2,   // 动力学，受重力/力/碰撞影响
};

// ---------------------------------------------------------------------------
// RigidBody2D — 2D 刚体
// 必须与 BoxCollider2D / CircleCollider2D 之一组合；若两者都没有，系统会
// 补一个 1x1 米的默认方形碰撞体，保证刚体有质量。
// ---------------------------------------------------------------------------
class RigidBody2D : public Component {
public:
    Body2DType body_type = Body2DType::Dynamic;

    math::Vector2f velocity = math::Vector2f(0.0f, 0.0f); // 引擎单位/秒
    float angular_velocity = 0.0f;                         // 弧度/秒
    float linear_damping = 0.0f;
    float angular_damping = 0.05f;
    float gravity_scale = 1.0f;
    bool fixed_rotation = false;
    bool continuous = false; // 连续碰撞检测（高速子弹）
    bool awake = true;

    // 运行时句柄：Box2D b2BodyId 打包值，0 表示尚未创建（不序列化）
    uint64_t runtime_body = 0;

    RigidBody2D() = default;
    const char* type() const override { return "RigidBody2D"; }

    void serialize(nlohmann::json& out) const override {
        out["body_type"] = static_cast<int>(body_type);
        out["velocity"] = { velocity.x, velocity.y };
        out["angular_velocity"] = angular_velocity;
        out["linear_damping"] = linear_damping;
        out["angular_damping"] = angular_damping;
        out["gravity_scale"] = gravity_scale;
        out["fixed_rotation"] = fixed_rotation;
        out["continuous"] = continuous;
        out["awake"] = awake;
    }

    void deserialize(const nlohmann::json& in) override {
        body_type = static_cast<Body2DType>(in.value("body_type", 2));
        auto v = in.value("velocity", std::vector<float>{0.0f, 0.0f});
        if (v.size() >= 2) velocity = math::Vector2f(v[0], v[1]);
        angular_velocity = in.value("angular_velocity", 0.0f);
        linear_damping = in.value("linear_damping", 0.0f);
        angular_damping = in.value("angular_damping", 0.05f);
        gravity_scale = in.value("gravity_scale", 1.0f);
        fixed_rotation = in.value("fixed_rotation", false);
        continuous = in.value("continuous", false);
        awake = in.value("awake", true);
    }

    // 运行状态（速度、角速度）跨 Play/Stop 保留无意义，热重载时丢弃。
    void snapshot_runtime_state(nlohmann::json& out) const override {
        out["runtime_body"] = runtime_body;
    }
    void restore_runtime_state(const nlohmann::json& in) override {
        runtime_body = in.value("runtime_body", static_cast<uint64_t>(0));
    }
};

// ---------------------------------------------------------------------------
// BoxCollider2D — 矩形碰撞体（Box2D polygon）
// size 为完整宽高（引擎单位），offset 为相对实体原点的本地偏移。
// ---------------------------------------------------------------------------
class BoxCollider2D : public Component {
public:
    math::Vector2f size = math::Vector2f(1.0f, 1.0f);
    math::Vector2f offset = math::Vector2f(0.0f, 0.0f);
    float rotation = 0.0f;    // 本地旋转（弧度）
    float density = 1.0f;     // kg/m²，决定刚体质量
    float friction = 0.3f;
    float restitution = 0.0f;
    bool is_sensor = false;

    BoxCollider2D() = default;
    const char* type() const override { return "BoxCollider2D"; }

    void serialize(nlohmann::json& out) const override {
        out["size"] = { size.x, size.y };
        out["offset"] = { offset.x, offset.y };
        out["rotation"] = rotation;
        out["density"] = density;
        out["friction"] = friction;
        out["restitution"] = restitution;
        out["is_sensor"] = is_sensor;
    }

    void deserialize(const nlohmann::json& in) override {
        auto s = in.value("size", std::vector<float>{1.0f, 1.0f});
        if (s.size() >= 2) size = math::Vector2f(s[0], s[1]);
        auto o = in.value("offset", std::vector<float>{0.0f, 0.0f});
        if (o.size() >= 2) offset = math::Vector2f(o[0], o[1]);
        rotation = in.value("rotation", 0.0f);
        density = in.value("density", 1.0f);
        friction = in.value("friction", 0.3f);
        restitution = in.value("restitution", 0.0f);
        is_sensor = in.value("is_sensor", false);
    }
};

// ---------------------------------------------------------------------------
// CircleCollider2D — 圆形碰撞体
// ---------------------------------------------------------------------------
class CircleCollider2D : public Component {
public:
    float radius = 0.5f;
    math::Vector2f offset = math::Vector2f(0.0f, 0.0f);
    float density = 1.0f;
    float friction = 0.3f;
    float restitution = 0.0f;
    bool is_sensor = false;

    CircleCollider2D() = default;
    const char* type() const override { return "CircleCollider2D"; }

    void serialize(nlohmann::json& out) const override {
        out["radius"] = radius;
        out["offset"] = { offset.x, offset.y };
        out["density"] = density;
        out["friction"] = friction;
        out["restitution"] = restitution;
        out["is_sensor"] = is_sensor;
    }

    void deserialize(const nlohmann::json& in) override {
        radius = in.value("radius", 0.5f);
        auto o = in.value("offset", std::vector<float>{0.0f, 0.0f});
        if (o.size() >= 2) offset = math::Vector2f(o[0], o[1]);
        density = in.value("density", 1.0f);
        friction = in.value("friction", 0.3f);
        restitution = in.value("restitution", 0.0f);
        is_sensor = in.value("is_sensor", false);
    }
};

// ---------------------------------------------------------------------------
// WindZone2D — 2D 风力区域
//
// 对区域内每个动态刚体施加 `force`（加速度，引擎单位/秒²）；`turbulence`
// 叠加随时间变化的正弦扰动，`falloff` 让风在区域边缘线性衰减到 0（0 = 全域
// 均匀，1 = 边缘完全无风）。风速按 `drag` 与物体速度的差值建模，可产生
// "顺风加速、逆风减速"的手感。
// ---------------------------------------------------------------------------
class WindZone2D : public Component {
public:
    math::Vector2f size = math::Vector2f(10.0f, 10.0f);   // 完整宽高
    math::Vector2f offset = math::Vector2f(0.0f, 0.0f);   // 相对实体原点
    math::Vector2f force = math::Vector2f(5.0f, 0.0f);    // 基础风加速度
    float turbulence = 0.0f;          // 湍流强度（加速度幅值）
    float turbulence_frequency = 1.0f; // 湍流变化频率（Hz）
    float falloff = 0.0f;             // 边缘衰减 [0,1]
    float drag = 0.0f;                // 速度阻尼系数：风力 ∝ drag*(风速-物速)
    bool affect_sleeping = false;     // 是否唤醒休眠刚体

    // 运行时相位（湍流用，不序列化）
    float runtime_phase = 0.0f;

    WindZone2D() = default;
    const char* type() const override { return "WindZone2D"; }

    void serialize(nlohmann::json& out) const override {
        out["size"] = { size.x, size.y };
        out["offset"] = { offset.x, offset.y };
        out["force"] = { force.x, force.y };
        out["turbulence"] = turbulence;
        out["turbulence_frequency"] = turbulence_frequency;
        out["falloff"] = falloff;
        out["drag"] = drag;
        out["affect_sleeping"] = affect_sleeping;
    }

    void deserialize(const nlohmann::json& in) override {
        auto s = in.value("size", std::vector<float>{10.0f, 10.0f});
        if (s.size() >= 2) size = math::Vector2f(s[0], s[1]);
        auto o = in.value("offset", std::vector<float>{0.0f, 0.0f});
        if (o.size() >= 2) offset = math::Vector2f(o[0], o[1]);
        auto f = in.value("force", std::vector<float>{5.0f, 0.0f});
        if (f.size() >= 2) force = math::Vector2f(f[0], f[1]);
        turbulence = in.value("turbulence", 0.0f);
        turbulence_frequency = in.value("turbulence_frequency", 1.0f);
        falloff = in.value("falloff", 0.0f);
        drag = in.value("drag", 0.0f);
        affect_sleeping = in.value("affect_sleeping", false);
    }
};

// ---------------------------------------------------------------------------
// BuoyancyArea2D — 2D 浮力/流体区域
//
// 采用阿基米德原理：物体浸入流体的体积比例 f 决定浮力大小
//     F_buoy = fluid_density * f * body_area * |g|      （方向与重力相反）
// 并叠加与相对流速成正比的线性阻力与角阻力，使物体最终稳定漂浮在
// surface_height 附近。fluid_density 与碰撞体 density 同量纲（kg/m²），
// 因此密度比直接决定物体浸没比例：density < fluid_density 时上浮。
//
// 水面为水平线 y = surface_height（世界坐标，引擎单位）；区域矩形只用于
// 限定作用范围（水平边界），垂直方向按水面判定浸没。
// ---------------------------------------------------------------------------
class BuoyancyArea2D : public Component {
public:
    math::Vector2f size = math::Vector2f(20.0f, 10.0f);  // 作用区域完整宽高
    math::Vector2f offset = math::Vector2f(0.0f, 0.0f);
    float surface_height = 0.0f;      // 世界坐标 y（水面）
    float fluid_density = 1000.0f;    // kg/m²
    float linear_drag = 2.0f;         // 相对流速的线性阻力系数
    float angular_drag = 1.0f;        // 角阻力系数
    math::Vector2f flow_velocity = math::Vector2f(0.0f, 0.0f); // 水流速度
    bool affects_sleeping = true;     // 是否唤醒休眠刚体

    BuoyancyArea2D() = default;
    const char* type() const override { return "BuoyancyArea2D"; }

    void serialize(nlohmann::json& out) const override {
        out["size"] = { size.x, size.y };
        out["offset"] = { offset.x, offset.y };
        out["surface_height"] = surface_height;
        out["fluid_density"] = fluid_density;
        out["linear_drag"] = linear_drag;
        out["angular_drag"] = angular_drag;
        out["flow_velocity"] = { flow_velocity.x, flow_velocity.y };
        out["affects_sleeping"] = affects_sleeping;
    }

    void deserialize(const nlohmann::json& in) override {
        auto s = in.value("size", std::vector<float>{20.0f, 10.0f});
        if (s.size() >= 2) size = math::Vector2f(s[0], s[1]);
        auto o = in.value("offset", std::vector<float>{0.0f, 0.0f});
        if (o.size() >= 2) offset = math::Vector2f(o[0], o[1]);
        surface_height = in.value("surface_height", 0.0f);
        fluid_density = in.value("fluid_density", 1000.0f);
        linear_drag = in.value("linear_drag", 2.0f);
        angular_drag = in.value("angular_drag", 1.0f);
        auto f = in.value("flow_velocity", std::vector<float>{0.0f, 0.0f});
        if (f.size() >= 2) flow_velocity = math::Vector2f(f[0], f[1]);
        affects_sleeping = in.value("affects_sleeping", true);
    }
};

} // namespace gryce_engine::components
