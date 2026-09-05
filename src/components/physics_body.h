#pragma once

#include <vector>

#include "components/component.h"
#include "assets/mesh_data.h"
#include "math/math.h"
#include "physics/physics_point.h"

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// PhysicsBody — 质点/点云物理组件
// 可手工添加物理点，或从 MeshData 顶点生成，用于柔体/布料预览。
// ---------------------------------------------------------------------------
class PhysicsBody : public Component {
public:
    std::vector<physics::PhysicsPoint> points;

    bool simulate = true;
    math::Vector3f gravity = math::Vector3f(0.0f, -9.8f, 0.0f);
    float damping = 0.99f;       // Verlet 阻尼
    float floor_y = -2.0f;       // 简单地面
    float restitution = 0.5f;    // 地面反弹系数

    PhysicsBody() = default;

    const char* type() const override { return "PhysicsBody"; }

    void serialize(nlohmann::json& out) const override {
        out["simulate"] = simulate;
        out["gravity"] = { gravity.x, gravity.y, gravity.z };
        out["damping"] = damping;
        out["floor_y"] = floor_y;
        out["restitution"] = restitution;
        nlohmann::json pts = nlohmann::json::array();
        for (const auto& p : points) {
            pts.push_back({
                {"pos", {p.position.x, p.position.y, p.position.z}},
                {"old", {p.old_position.x, p.old_position.y, p.old_position.z}},
                {"mass", p.mass},
                {"pinned", p.pinned}
            });
        }
        out["points"] = pts;
    }

    void deserialize(const nlohmann::json& in) override {
        simulate = in.value("simulate", simulate);
        auto g = in.value("gravity", std::vector<float>{0.0f, -9.8f, 0.0f});
        if (g.size() >= 3) gravity = math::Vector3f(g[0], g[1], g[2]);
        damping = in.value("damping", damping);
        floor_y = in.value("floor_y", floor_y);
        restitution = in.value("restitution", restitution);

        points.clear();
        auto pts = in.value("points", nlohmann::json::array());
        for (const auto& p : pts) {
            auto pos = p.value("pos", std::vector<float>{0, 0, 0});
            auto old = p.value("old", pos);
            if (pos.size() >= 3) {
                physics::PhysicsPoint point;
                point.position = math::Vector3f(pos[0], pos[1], pos[2]);
                point.old_position = old.size() >= 3
                    ? math::Vector3f(old[0], old[1], old[2])
                    : point.position;
                point.mass = p.value("mass", 1.0f);
                point.pinned = p.value("pinned", false);
                points.push_back(point);
            }
        }
    }

    // 从网格顶点生成物理点（去重位置）
    void from_mesh(const assets::MeshData& mesh, float mass = 1.0f) {
        points.clear();
        points.reserve(mesh.vertices.size());
        for (const auto& v : mesh.vertices) {
            points.emplace_back(v.position, mass, false);
        }
    }

    void add_point(const math::Vector3f& pos, float mass = 1.0f, bool pinned = false) {
        points.emplace_back(pos, mass, pinned);
    }
};

} // namespace gryce_engine::components
