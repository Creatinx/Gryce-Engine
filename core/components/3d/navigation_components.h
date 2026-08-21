#pragma once

#include "components/component.h"
#include "math/math.h"
#include "scene/entity.h"

#include <cmath>
#include <string>
#include <vector>

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// NavigationMesh3D — 3D 导航网格区域（数据层；后续接入 Recast 烘焙）
// ---------------------------------------------------------------------------
class NavigationMesh3D : public Component {
public:
    std::string navmesh_path;
    math::Vector3f cell_size = math::Vector3f(0.3f, 0.3f, 0.3f);
    float agent_radius = 0.5f;
    float agent_height = 1.8f;
    float agent_max_slope = 45.0f;
    bool bake_dynamic = false;

    NavigationMesh3D() = default;
    const char* type() const override { return "NavigationMesh3D"; }

    void serialize(nlohmann::json& out) const override {
        out["navmesh_path"] = navmesh_path;
        out["cell_size"] = { cell_size.x, cell_size.y, cell_size.z };
        out["agent_radius"] = agent_radius;
        out["agent_height"] = agent_height;
        out["agent_max_slope"] = agent_max_slope;
        out["bake_dynamic"] = bake_dynamic;
    }
    void deserialize(const nlohmann::json& in) override {
        navmesh_path = in.value("navmesh_path", "");
        auto c = in.value("cell_size", std::vector<float>{0.3f, 0.3f, 0.3f});
        if (c.size() >= 3) cell_size = math::Vector3f(c[0], c[1], c[2]);
        agent_radius = in.value("agent_radius", 0.5f);
        agent_height = in.value("agent_height", 1.8f);
        agent_max_slope = in.value("agent_max_slope", 45.0f);
        bake_dynamic = in.value("bake_dynamic", false);
    }
};

// ---------------------------------------------------------------------------
// NavMeshAgent3D — 3D 寻路代理
// 当前实现为占位：set_target() 生成直线路径并按 speed 沿路径移动；
// 完整 NavMesh 路径查询（Recast/Detour）接入后替换寻路部分。
// ---------------------------------------------------------------------------
class NavMeshAgent3D : public Component {
public:
    float radius = 0.5f;
    float height = 1.8f;
    float speed = 3.5f;
    float angular_speed = 360.0f;
    bool avoidance_enabled = true;

    // 运行时目标与路径（不序列化）
    math::Vector3f target_position = math::Vector3f::zero();
    bool target_valid = false;
    bool path_following = false;
    int current_waypoint = 0;
    std::vector<math::Vector3f> waypoints;

    NavMeshAgent3D() = default;
    const char* type() const override { return "NavMeshAgent3D"; }

    void set_target(const math::Vector3f& target) {
        target_position = target;
        target_valid = true;
        waypoints.clear();
        if (owner_ && owner_->transform()) {
            waypoints.push_back(owner_->transform()->position);
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
            const math::Vector3f goal = waypoints[current_waypoint];
            const math::Vector3f delta = goal - t->position;
            const float dist = delta.length();
            if (dist < 0.05f) {
                ++current_waypoint;
                if (current_waypoint >= static_cast<int>(waypoints.size())) {
                    t->position = goal;
                    path_following = false;
                    target_valid = false;
                }
                continue;
            }
            const float step = std::max(0.0f, speed * dt);
            if (dist <= step) {
                t->position = goal;
                ++current_waypoint;
                if (current_waypoint >= static_cast<int>(waypoints.size())) {
                    path_following = false;
                    target_valid = false;
                }
                continue;
            }
            t->position = t->position + delta * (step / dist);
            return;
        }
    }

    void serialize(nlohmann::json& out) const override {
        out["radius"] = radius;
        out["height"] = height;
        out["speed"] = speed;
        out["angular_speed"] = angular_speed;
        out["avoidance_enabled"] = avoidance_enabled;
    }
    void deserialize(const nlohmann::json& in) override {
        radius = in.value("radius", 0.5f);
        height = in.value("height", 1.8f);
        speed = in.value("speed", 3.5f);
        angular_speed = in.value("angular_speed", 360.0f);
        avoidance_enabled = in.value("avoidance_enabled", true);
    }
};

// ---------------------------------------------------------------------------
// NavMeshObstacle3D — 3D 寻路障碍物
// ---------------------------------------------------------------------------
class NavMeshObstacle3D : public Component {
public:
    float radius = 0.5f;
    float height = 2.0f;
    bool carve = true;

    NavMeshObstacle3D() = default;
    const char* type() const override { return "NavMeshObstacle3D"; }

    void serialize(nlohmann::json& out) const override {
        out["radius"] = radius;
        out["height"] = height;
        out["carve"] = carve;
    }
    void deserialize(const nlohmann::json& in) override {
        radius = in.value("radius", 0.5f);
        height = in.value("height", 2.0f);
        carve = in.value("carve", true);
    }
};

} // namespace gryce_engine::components
