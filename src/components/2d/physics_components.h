#pragma once

#include "components/component.h"
#include "components/2d/component_2d.h"
#include "math/math.h"
#include "scene/uuid.h"

#include <string>
#include <vector>

namespace gryce_engine::components::d2 {

// ---------------------------------------------------------------------------
// PolygonCollider2D — 2D 多边形碰撞体（Box2D 凸多边形）
// ---------------------------------------------------------------------------
class PolygonCollider2D : public Component2D {
public:
    std::vector<math::Vector2f> points;
    math::Vector2f offset = math::Vector2f::zero();
    bool is_trigger = false;

    PolygonCollider2D() {
        points = { math::Vector2f(-0.5f, -0.5f), math::Vector2f(0.5f, -0.5f),
                   math::Vector2f(0.5f, 0.5f), math::Vector2f(-0.5f, 0.5f) };
    }
    const char* type() const override { return "PolygonCollider2D"; }
    void draw(render::IRenderer2D* /*renderer*/) override {}

    void serialize(nlohmann::json& out) const override {
        Component2D::serialize_base(out);
        out["offset"] = { offset.x, offset.y };
        out["is_trigger"] = is_trigger;
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& p : points) arr.push_back({ p.x, p.y });
        out["points"] = std::move(arr);
    }
    void deserialize(const nlohmann::json& in) override {
        Component2D::deserialize_base(in);
        auto o = in.value("offset", std::vector<float>{0, 0});
        if (o.size() >= 2) offset = math::Vector2f(o[0], o[1]);
        is_trigger = in.value("is_trigger", false);
        points.clear();
        if (in.contains("points") && in["points"].is_array()) {
            for (const auto& item : in["points"]) {
                if (item.is_array() && item.size() >= 2) {
                    points.emplace_back(item[0].get<float>(), item[1].get<float>());
                }
            }
        }
        if (points.size() < 3) {
            points = { math::Vector2f(-0.5f, -0.5f), math::Vector2f(0.5f, -0.5f),
                       math::Vector2f(0.5f, 0.5f), math::Vector2f(-0.5f, 0.5f) };
        }
    }
};

// ---------------------------------------------------------------------------
// CapsuleCollider2D — 2D 胶囊碰撞体（Box2D v3 原生）
// ---------------------------------------------------------------------------
class CapsuleCollider2D : public Component2D {
public:
    float radius = 0.25f;
    float height = 1.0f;
    math::Vector2f offset = math::Vector2f::zero();
    bool is_trigger = false;

    CapsuleCollider2D() = default;
    const char* type() const override { return "CapsuleCollider2D"; }
    void draw(render::IRenderer2D* /*renderer*/) override {}

    void serialize(nlohmann::json& out) const override {
        Component2D::serialize_base(out);
        out["radius"] = radius;
        out["height"] = height;
        out["offset"] = { offset.x, offset.y };
        out["is_trigger"] = is_trigger;
    }
    void deserialize(const nlohmann::json& in) override {
        Component2D::deserialize_base(in);
        radius = in.value("radius", 0.25f);
        height = in.value("height", 1.0f);
        auto o = in.value("offset", std::vector<float>{0, 0});
        if (o.size() >= 2) offset = math::Vector2f(o[0], o[1]);
        is_trigger = in.value("is_trigger", false);
    }
};

// ---------------------------------------------------------------------------
// EdgeCollider2D — 2D 线段碰撞体（平台边缘、墙壁；Box2D Segment）
// ---------------------------------------------------------------------------
class EdgeCollider2D : public Component2D {
public:
    math::Vector2f p1 = math::Vector2f(-0.5f, 0.0f);
    math::Vector2f p2 = math::Vector2f(0.5f, 0.0f);
    bool is_trigger = false;

    EdgeCollider2D() = default;
    const char* type() const override { return "EdgeCollider2D"; }
    void draw(render::IRenderer2D* /*renderer*/) override {}

    void serialize(nlohmann::json& out) const override {
        Component2D::serialize_base(out);
        out["p1"] = { p1.x, p1.y };
        out["p2"] = { p2.x, p2.y };
        out["is_trigger"] = is_trigger;
    }
    void deserialize(const nlohmann::json& in) override {
        Component2D::deserialize_base(in);
        auto a = in.value("p1", std::vector<float>{-0.5f, 0});
        if (a.size() >= 2) p1 = math::Vector2f(a[0], a[1]);
        auto b = in.value("p2", std::vector<float>{0.5f, 0});
        if (b.size() >= 2) p2 = math::Vector2f(b[0], b[1]);
        is_trigger = in.value("is_trigger", false);
    }
};

// ---------------------------------------------------------------------------
// TileMapCollider — 瓦片地图碰撞体（由同实体 Tilemap 自动生成碰撞）
// ---------------------------------------------------------------------------
class TileMapCollider : public Component2D {
public:
    bool one_way = false;
    bool is_trigger = false;

    TileMapCollider() = default;
    const char* type() const override { return "TileMapCollider"; }
    void draw(render::IRenderer2D* /*renderer*/) override {}

    void serialize(nlohmann::json& out) const override {
        Component2D::serialize_base(out);
        out["one_way"] = one_way;
        out["is_trigger"] = is_trigger;
    }
    void deserialize(const nlohmann::json& in) override {
        Component2D::deserialize_base(in);
        one_way = in.value("one_way", false);
        is_trigger = in.value("is_trigger", false);
    }
};

// ---------------------------------------------------------------------------
// Area2D — 2D 触发区域（拾取、陷阱、传送门；Enter/Exit 事件）
// ---------------------------------------------------------------------------
class Area2D : public Component2D {
public:
    bool is_box = true;
    math::Vector2f size = math::Vector2f::one();
    float radius = 0.5f;
    bool monitorable = true;
    bool monitor = true;

    Area2D() = default;
    const char* type() const override { return "Area2D"; }
    void draw(render::IRenderer2D* /*renderer*/) override {}

    void serialize(nlohmann::json& out) const override {
        Component2D::serialize_base(out);
        out["is_box"] = is_box;
        out["size"] = { size.x, size.y };
        out["radius"] = radius;
        out["monitorable"] = monitorable;
        out["monitor"] = monitor;
    }
    void deserialize(const nlohmann::json& in) override {
        Component2D::deserialize_base(in);
        is_box = in.value("is_box", true);
        auto s = in.value("size", std::vector<float>{1, 1});
        if (s.size() >= 2) size = math::Vector2f(s[0], s[1]);
        radius = in.value("radius", 0.5f);
        monitorable = in.value("monitorable", true);
        monitor = in.value("monitor", true);
    }
};

// ---------------------------------------------------------------------------
// RayCast2D — 2D 射线检测组件（每帧由 PhysicsSystem2D 填充结果）
// ---------------------------------------------------------------------------
class RayCast2D : public Component2D {
public:
    math::Vector2f direction = math::Vector2f(0.0f, -1.0f);
    float max_distance = 10.0f;

    // 运行时结果（只读）
    bool hit = false;
    math::Vector2f hit_point = math::Vector2f::zero();
    math::Vector2f hit_normal = math::Vector2f::zero();
    float hit_distance = 0.0f;

    RayCast2D() = default;
    const char* type() const override { return "RayCast2D"; }
    void draw(render::IRenderer2D* /*renderer*/) override {}

    void serialize(nlohmann::json& out) const override {
        Component2D::serialize_base(out);
        out["direction"] = { direction.x, direction.y };
        out["max_distance"] = max_distance;
    }
    void deserialize(const nlohmann::json& in) override {
        Component2D::deserialize_base(in);
        auto d = in.value("direction", std::vector<float>{0, -1});
        if (d.size() >= 2) direction = math::Vector2f(d[0], d[1]);
        max_distance = in.value("max_distance", 10.0f);
    }
};

// ---------------------------------------------------------------------------
// 专用 2D 关节组件（连接两个 RigidBody2D 实体；由 PhysicsSystem2D 消费）
// ---------------------------------------------------------------------------
struct Joint2DCommon {
    scene::UUID body_a_uuid;
    scene::UUID body_b_uuid;
    math::Vector2f anchor_a = math::Vector2f::zero();
    math::Vector2f anchor_b = math::Vector2f::zero();
    bool collide_connected = false;

    void serialize_common(nlohmann::json& out) const {
        out["body_a_uuid"] = body_a_uuid.str();
        out["body_b_uuid"] = body_b_uuid.str();
        out["anchor_a"] = { anchor_a.x, anchor_a.y };
        out["anchor_b"] = { anchor_b.x, anchor_b.y };
        out["collide_connected"] = collide_connected;
    }
    void deserialize_common(const nlohmann::json& in) {
        body_a_uuid = scene::UUID(in.value("body_a_uuid", ""));
        body_b_uuid = scene::UUID(in.value("body_b_uuid", ""));
        auto a = in.value("anchor_a", std::vector<float>{0, 0});
        if (a.size() >= 2) anchor_a = math::Vector2f(a[0], a[1]);
        auto b = in.value("anchor_b", std::vector<float>{0, 0});
        if (b.size() >= 2) anchor_b = math::Vector2f(b[0], b[1]);
        collide_connected = in.value("collide_connected", false);
    }
};

class HingeJoint2D : public Component, public Joint2DCommon {
public:
    HingeJoint2D() = default;
    const char* type() const override { return "HingeJoint2D"; }
    void serialize(nlohmann::json& out) const override { serialize_common(out); }
    void deserialize(const nlohmann::json& in) override { deserialize_common(in); }
};

class WeldJoint2D : public Component, public Joint2DCommon {
public:
    WeldJoint2D() = default;
    const char* type() const override { return "WeldJoint2D"; }
    void serialize(nlohmann::json& out) const override { serialize_common(out); }
    void deserialize(const nlohmann::json& in) override { deserialize_common(in); }
};

class PrismaticJoint2D : public Component, public Joint2DCommon {
public:
    math::Vector2f axis = math::Vector2f(1.0f, 0.0f);
    bool enable_limit = false;
    float lower_translation = 0.0f;
    float upper_translation = 1.0f;
    bool enable_motor = false;
    float motor_speed = 0.0f;
    float max_motor_force = 100.0f;

    PrismaticJoint2D() = default;
    const char* type() const override { return "PrismaticJoint2D"; }
    void serialize(nlohmann::json& out) const override {
        serialize_common(out);
        out["axis"] = { axis.x, axis.y };
        out["enable_limit"] = enable_limit;
        out["lower_translation"] = lower_translation;
        out["upper_translation"] = upper_translation;
        out["enable_motor"] = enable_motor;
        out["motor_speed"] = motor_speed;
        out["max_motor_force"] = max_motor_force;
    }
    void deserialize(const nlohmann::json& in) override {
        deserialize_common(in);
        auto a = in.value("axis", std::vector<float>{1, 0});
        if (a.size() >= 2) axis = math::Vector2f(a[0], a[1]);
        enable_limit = in.value("enable_limit", false);
        lower_translation = in.value("lower_translation", 0.0f);
        upper_translation = in.value("upper_translation", 1.0f);
        enable_motor = in.value("enable_motor", false);
        motor_speed = in.value("motor_speed", 0.0f);
        max_motor_force = in.value("max_motor_force", 100.0f);
    }
};

class WheelJoint2D : public Component, public Joint2DCommon {
public:
    math::Vector2f axis = math::Vector2f(0.0f, 1.0f);
    bool enable_motor = false;
    float motor_speed = 0.0f;
    float max_motor_torque = 20.0f;
    float frequency = 2.0f;
    float damping = 0.5f;

    WheelJoint2D() = default;
    const char* type() const override { return "WheelJoint2D"; }
    void serialize(nlohmann::json& out) const override {
        serialize_common(out);
        out["axis"] = { axis.x, axis.y };
        out["enable_motor"] = enable_motor;
        out["motor_speed"] = motor_speed;
        out["max_motor_torque"] = max_motor_torque;
        out["frequency"] = frequency;
        out["damping"] = damping;
    }
    void deserialize(const nlohmann::json& in) override {
        deserialize_common(in);
        auto a = in.value("axis", std::vector<float>{0, 1});
        if (a.size() >= 2) axis = math::Vector2f(a[0], a[1]);
        enable_motor = in.value("enable_motor", false);
        motor_speed = in.value("motor_speed", 0.0f);
        max_motor_torque = in.value("max_motor_torque", 20.0f);
        frequency = in.value("frequency", 2.0f);
        damping = in.value("damping", 0.5f);
    }
};

class RopeJoint2D : public Component, public Joint2DCommon {
public:
    float length = 1.0f;

    RopeJoint2D() = default;
    const char* type() const override { return "RopeJoint2D"; }
    void serialize(nlohmann::json& out) const override {
        serialize_common(out);
        out["length"] = length;
    }
    void deserialize(const nlohmann::json& in) override {
        deserialize_common(in);
        length = in.value("length", 1.0f);
    }
};

} // namespace gryce_engine::components::d2
