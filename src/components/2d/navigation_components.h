#pragma once

#include "components/2d/component_2d.h"
#include "math/math.h"
#include "scene/entity.h"

#include <cmath>
#include <string>
#include <vector>

namespace gryce_engine::components::d2 {

// ---------------------------------------------------------------------------
// NavigationRegion2D — 2D 导航区域（A* 可行走区域；数据层）
// ---------------------------------------------------------------------------
class NavigationRegion2D : public Component2D {
public:
    std::vector<math::Vector2f> polygon;
    std::string navmesh_path;
    bool bake_navigation = true;

    NavigationRegion2D() {
        polygon = { math::Vector2f(-5, -5), math::Vector2f(5, -5),
                    math::Vector2f(5, 5), math::Vector2f(-5, 5) };
    }
    const char* type() const override { return "NavigationRegion2D"; }
    void draw(render::IRenderer2D* /*renderer*/) override {}

    void serialize(nlohmann::json& out) const override {
        Component2D::serialize_base(out);
        out["navmesh_path"] = navmesh_path;
        out["bake_navigation"] = bake_navigation;
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& p : polygon) arr.push_back({ p.x, p.y });
        out["polygon"] = std::move(arr);
    }
    void deserialize(const nlohmann::json& in) override {
        Component2D::deserialize_base(in);
        navmesh_path = in.value("navmesh_path", "");
        bake_navigation = in.value("bake_navigation", true);
        polygon.clear();
        if (in.contains("polygon") && in["polygon"].is_array()) {
            for (const auto& item : in["polygon"]) {
                if (item.is_array() && item.size() >= 2) {
                    polygon.emplace_back(item[0].get<float>(), item[1].get<float>());
                }
            }
        }
    }
};

// ---------------------------------------------------------------------------
// NavigationAgent2D — 2D 寻路代理
// 当前为占位：set_target() 生成直线路径并按 speed 移动；A* 接入后替换。
// ---------------------------------------------------------------------------
class NavigationAgent2D : public Component2D {
public:
    float radius = 0.25f;
    float speed = 3.0f;
    bool avoidance_enabled = true;

    // 运行时（不序列化）
    math::Vector2f target_position = math::Vector2f::zero();
    bool target_valid = false;
    bool path_following = false;
    int current_waypoint = 0;
    std::vector<math::Vector2f> waypoints;

    NavigationAgent2D() = default;
    const char* type() const override { return "NavigationAgent2D"; }
    void draw(render::IRenderer2D* /*renderer*/) override {}

    void set_target(const math::Vector2f& target) {
        target_position = target;
        target_valid = true;
        waypoints.clear();
        if (owner_ && owner_->transform()) {
            waypoints.emplace_back(owner_->transform()->position.x, owner_->transform()->position.y);
        }
        waypoints.push_back(target);
        current_waypoint = 0;
        path_following = true;
    }

    void on_update(float dt) override {
        if (!target_valid || !path_following || !owner_ || !owner_->transform()) return;
        auto* t = owner_->transform();
        while (true) {
            if (waypoints.empty() || current_waypoint >= static_cast<int>(waypoints.size())) {
                path_following = false;
                target_valid = false;
                return;
            }
            const math::Vector2f goal = waypoints[current_waypoint];
            math::Vector2f pos(t->position.x, t->position.y);
            const math::Vector2f delta = goal - pos;
            const float dist = delta.length();
            if (dist < 0.03f) {
                ++current_waypoint;
                if (current_waypoint >= static_cast<int>(waypoints.size())) {
                    t->position.x = goal.x;
                    t->position.y = goal.y;
                    path_following = false;
                    target_valid = false;
                }
                continue;
            }
            const float step = std::max(0.0f, speed * dt);
            if (dist <= step) {
                t->position.x = goal.x;
                t->position.y = goal.y;
                ++current_waypoint;
                if (current_waypoint >= static_cast<int>(waypoints.size())) {
                    path_following = false;
                    target_valid = false;
                }
                continue;
            }
            const math::Vector2f move = delta * (step / dist);
            t->position.x += move.x;
            t->position.y += move.y;
            return;
        }
    }

    void serialize(nlohmann::json& out) const override {
        Component2D::serialize_base(out);
        out["radius"] = radius;
        out["speed"] = speed;
        out["avoidance_enabled"] = avoidance_enabled;
    }
    void deserialize(const nlohmann::json& in) override {
        Component2D::deserialize_base(in);
        radius = in.value("radius", 0.25f);
        speed = in.value("speed", 3.0f);
        avoidance_enabled = in.value("avoidance_enabled", true);
    }
};

} // namespace gryce_engine::components::d2
