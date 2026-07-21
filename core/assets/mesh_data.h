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
// MeshMaterialData — 模型文件自带的材质数据（OBJ MTL / assimp 提取）
// MeshRenderer 在上传时把其中仍为默认值的字段合并进组件 Material。
// ---------------------------------------------------------------------------
struct MeshMaterialData {
    bool valid = false;

    math::Vector3f albedo_color = math::Vector3f::one();
    math::Vector3f emissive_color = math::Vector3f::zero();
    float opacity = 1.0f;
    float roughness = 0.5f;
    float metallic = 0.0f;

    // 贴图路径（相对模型文件目录解析后的完整路径）
    std::string albedo_map;
    std::string normal_map;
    std::string emissive_map;
    std::string roughness_map;
    std::string metallic_map;
    std::string ao_map;
};

// ---------------------------------------------------------------------------
// MeshData — 网格资源数据（CPU 侧）
// ---------------------------------------------------------------------------
struct MeshData : public Asset {
    std::string name;
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
    MeshMaterialData material;

    const char* type() const override { return "MeshData"; }

    size_t memory_size() const override {
        return vertices.size() * sizeof(MeshVertex) +
               indices.size() * sizeof(uint32_t) +
               name.capacity() * sizeof(char) +
               material.albedo_map.capacity() * sizeof(char) +
               material.normal_map.capacity() * sizeof(char) +
               material.emissive_map.capacity() * sizeof(char) +
               material.roughness_map.capacity() * sizeof(char) +
               material.metallic_map.capacity() * sizeof(char) +
               material.ao_map.capacity() * sizeof(char);
    }

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
