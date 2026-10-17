#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "assets/asset.h"
#include "assets/material_rules.h"
#include "math/math.h"

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

    // 文件里的材质名（随导入日志输出；空表示格式没给）
    std::string name;
    // 推导出的表面类型（金属/非金属/半透明/自发光），只用于日志与工具展示；
    // 渲染行为由 metallic / roughness / opacity / emissive 决定。
    SurfaceType surface_type = SurfaceType::Dielectric;

    math::Vector3f albedo_color = math::Vector3f::one();
    math::Vector3f emissive_color = math::Vector3f::zero();
    float opacity = 1.0f;
    float roughness = 0.5f;
    float metallic = 0.0f;
    // 文件声明双面（布料/薄片/树叶卡片）。导入时与半透明一起推导。
    bool two_sided = false;

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
               material.name.capacity() * sizeof(char) +
               material.albedo_map.capacity() * sizeof(char) +
               material.normal_map.capacity() * sizeof(char) +
               material.emissive_map.capacity() * sizeof(char) +
               material.roughness_map.capacity() * sizeof(char) +
               material.metallic_map.capacity() * sizeof(char) +
               material.ao_map.capacity() * sizeof(char);
    }

    bool empty() const { return vertices.empty(); }
};

} // namespace gryce_engine::assets
