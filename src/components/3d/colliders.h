#pragma once

#include "components/component.h"
#include "math/math.h"

#include <string>

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// CapsuleCollider — 胶囊碰撞体（角色/人体常见；Jolt 原生支持）
// ---------------------------------------------------------------------------
class CapsuleCollider : public Component {
public:
    float radius = 0.5f;
    float height = 2.0f;
    math::Vector3f center = math::Vector3f::zero();
    bool is_trigger = false;
    int direction = 1; // 0=X, 1=Y, 2=Z

    CapsuleCollider() = default;
    const char* type() const override { return "CapsuleCollider"; }

    void serialize(nlohmann::json& out) const override {
        out["radius"] = radius;
        out["height"] = height;
        out["center"] = { center.x, center.y, center.z };
        out["is_trigger"] = is_trigger;
        out["direction"] = direction;
    }
    void deserialize(const nlohmann::json& in) override {
        radius = in.value("radius", 0.5f);
        height = in.value("height", 2.0f);
        auto c = in.value("center", std::vector<float>{0, 0, 0});
        if (c.size() >= 3) center = math::Vector3f(c[0], c[1], c[2]);
        is_trigger = in.value("is_trigger", false);
        direction = in.value("direction", 1);
    }
};

// ---------------------------------------------------------------------------
// CylinderCollider — 圆柱碰撞体（桶、管道）
// ---------------------------------------------------------------------------
class CylinderCollider : public Component {
public:
    float radius = 0.5f;
    float height = 1.0f;
    math::Vector3f center = math::Vector3f::zero();
    bool is_trigger = false;

    CylinderCollider() = default;
    const char* type() const override { return "CylinderCollider"; }

    void serialize(nlohmann::json& out) const override {
        out["radius"] = radius;
        out["height"] = height;
        out["center"] = { center.x, center.y, center.z };
        out["is_trigger"] = is_trigger;
    }
    void deserialize(const nlohmann::json& in) override {
        radius = in.value("radius", 0.5f);
        height = in.value("height", 1.0f);
        auto c = in.value("center", std::vector<float>{0, 0, 0});
        if (c.size() >= 3) center = math::Vector3f(c[0], c[1], c[2]);
        is_trigger = in.value("is_trigger", false);
    }
};

// ---------------------------------------------------------------------------
// ConvexMeshCollider — 凸包网格碰撞体（任意网格的低成本近似）
// ---------------------------------------------------------------------------
class ConvexMeshCollider : public Component {
public:
    std::string mesh_path;
    math::Vector3f center = math::Vector3f::zero();
    bool is_trigger = false;

    ConvexMeshCollider() = default;
    const char* type() const override { return "ConvexMeshCollider"; }

    void serialize(nlohmann::json& out) const override {
        out["mesh_path"] = mesh_path;
        out["center"] = { center.x, center.y, center.z };
        out["is_trigger"] = is_trigger;
    }
    void deserialize(const nlohmann::json& in) override {
        mesh_path = in.value("mesh_path", "");
        auto c = in.value("center", std::vector<float>{0, 0, 0});
        if (c.size() >= 3) center = math::Vector3f(c[0], c[1], c[2]);
        is_trigger = in.value("is_trigger", false);
    }
};

// ---------------------------------------------------------------------------
// MeshCollider — 静态三角网格碰撞体（关卡、场景几何）
// ---------------------------------------------------------------------------
class MeshCollider : public Component {
public:
    std::string mesh_path;
    bool is_trigger = false;
    bool convex = false;

    MeshCollider() = default;
    const char* type() const override { return "MeshCollider"; }

    void serialize(nlohmann::json& out) const override {
        out["mesh_path"] = mesh_path;
        out["is_trigger"] = is_trigger;
        out["convex"] = convex;
    }
    void deserialize(const nlohmann::json& in) override {
        mesh_path = in.value("mesh_path", "");
        is_trigger = in.value("is_trigger", false);
        convex = in.value("convex", false);
    }
};

// ---------------------------------------------------------------------------
// TriggerVolume — 触发器区域（Box/Sphere，Enter/Exit 事件）
// ---------------------------------------------------------------------------
class TriggerVolume : public Component {
public:
    bool is_box = true;
    math::Vector3f size = math::Vector3f::one();
    float radius = 0.5f;
    math::Vector3f center = math::Vector3f::zero();

    TriggerVolume() = default;
    const char* type() const override { return "TriggerVolume"; }

    void serialize(nlohmann::json& out) const override {
        out["is_box"] = is_box;
        out["size"] = { size.x, size.y, size.z };
        out["radius"] = radius;
        out["center"] = { center.x, center.y, center.z };
    }
    void deserialize(const nlohmann::json& in) override {
        is_box = in.value("is_box", true);
        auto s = in.value("size", std::vector<float>{1, 1, 1});
        if (s.size() >= 3) size = math::Vector3f(s[0], s[1], s[2]);
        radius = in.value("radius", 0.5f);
        auto c = in.value("center", std::vector<float>{0, 0, 0});
        if (c.size() >= 3) center = math::Vector3f(c[0], c[1], c[2]);
    }
};

// ---------------------------------------------------------------------------
// WheelCollider — 车轮碰撞体（车辆；数据层，车辆系统后续消费）
// ---------------------------------------------------------------------------
class WheelCollider : public Component {
public:
    float radius = 0.35f;
    float suspension_rest_length = 0.2f;
    float suspension_stiffness = 100.0f;
    float mass = 20.0f;
    float friction = 1.0f;
    float motor_torque = 0.0f;
    float brake_torque = 0.0f;
    float steer_angle = 0.0f;

    WheelCollider() = default;
    const char* type() const override { return "WheelCollider"; }

    void serialize(nlohmann::json& out) const override {
        out["radius"] = radius;
        out["suspension_rest_length"] = suspension_rest_length;
        out["suspension_stiffness"] = suspension_stiffness;
        out["mass"] = mass;
        out["friction"] = friction;
        out["motor_torque"] = motor_torque;
        out["brake_torque"] = brake_torque;
        out["steer_angle"] = steer_angle;
    }
    void deserialize(const nlohmann::json& in) override {
        radius = in.value("radius", 0.35f);
        suspension_rest_length = in.value("suspension_rest_length", 0.2f);
        suspension_stiffness = in.value("suspension_stiffness", 100.0f);
        mass = in.value("mass", 20.0f);
        friction = in.value("friction", 1.0f);
        motor_torque = in.value("motor_torque", 0.0f);
        brake_torque = in.value("brake_torque", 0.0f);
        steer_angle = in.value("steer_angle", 0.0f);
    }
};

} // namespace gryce_engine::components
