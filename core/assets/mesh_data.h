#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "assets/asset.h"
#include "math/math.h"
#include "physics/physics_point.h"

namespace gryce_engine::assets {

// ---------------------------------------------------------------------------
// MeshVertex — 通用网格顶点（支持法线贴图 Tangent 空间）
// ---------------------------------------------------------------------------
struct MeshVertex {
    math::Vector3f position;
    math::Vector3f normal;
    math::Vector3f tangent;
    math::Vector2f uv;
    math::Vector3f color;

    MeshVertex() = default;
    MeshVertex(const math::Vector3f& p, const math::Vector3f& n, const math::Vector3f& tang,
               const math::Vector2f& t, const math::Vector3f& c)
        : position(p), normal(n), tangent(tang), uv(t), color(c) {}
};

// ---------------------------------------------------------------------------
// MeshData — 网格资源数据（CPU 侧）
// ---------------------------------------------------------------------------
struct MeshData : public Asset {
    std::string name;
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;

    const char* type() const override { return "MeshData"; }

    bool empty() const { return vertices.empty(); }

    // 导出为物理点位（预留接口）
    std::vector<physics::PhysicsPoint> to_physics_points(float mass = 1.0f) const {
        std::vector<physics::PhysicsPoint> points;
        points.reserve(vertices.size());
        for (const auto& v : vertices) {
            points.emplace_back(v.position, mass, false);
        }
        return points;
    }
};

} // namespace gryce_engine::assets
