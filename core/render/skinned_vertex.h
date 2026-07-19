#pragma once

#include <cstdint>
#include <vector>

#include "assets/skinned_mesh_data.h"
#include "render/mesh.h"

namespace gryce_engine::render {

// ---------------------------------------------------------------------------
// 蒙皮顶点 GPU 布局（GL / VK 两端共享）
//
// 在 MeshVertex（56B）之后追加：
//   location 5 = bone ids（4×uint32，整数属性）
//   location 6 = weights （4×float）
// 与 assets::BoneInfluence 的内存布局一一对应。
// ---------------------------------------------------------------------------

// palette 骨骼数上限（GL uniform 数组 / VK palette UBO 共用）
inline constexpr uint32_t k_max_skinning_bones = 128;

struct SkinnedVertexGPU {
    float px, py, pz;        // location 0: position
    float nx, ny, nz;        // location 1: normal
    float tx, ty, tz;        // location 2: tangent
    float u, v;              // location 3: uv
    float r, g, b;           // location 4: color
    uint32_t bone_ids[4];    // location 5: bone ids（UInt4，整数属性）
    float weights[4];        // location 6: weights（Float4）
};

static_assert(sizeof(SkinnedVertexGPU) == 88, "SkinnedVertexGPU 必须为 88 字节（VK 管线硬编码 stride）");

// SkinnedMeshData（MeshVertex + BoneInfluence 平行数组）→ 交错 GPU 顶点。
// bone_influences 为空或数量与 vertices 不符时按无蒙皮处理（ids/weights 全零，
// 着色器侧贡献为 0，顶点保持原位）。
inline std::vector<SkinnedVertexGPU> build_skinned_vertices(const assets::SkinnedMeshData& data) {
    std::vector<SkinnedVertexGPU> out;
    out.reserve(data.vertices.size());
    const bool has_skin = data.bone_influences.size() == data.vertices.size();
    for (size_t i = 0; i < data.vertices.size(); ++i) {
        const auto& sv = data.vertices[i];
        SkinnedVertexGPU v{
            sv.position.x, sv.position.y, sv.position.z,
            sv.normal.x, sv.normal.y, sv.normal.z,
            sv.tangent.x, sv.tangent.y, sv.tangent.z,
            sv.uv.x, sv.uv.y,
            sv.color.x, sv.color.y, sv.color.z,
            {0u, 0u, 0u, 0u},
            {0.0f, 0.0f, 0.0f, 0.0f}
        };
        if (has_skin) {
            const auto& bi = data.bone_influences[i];
            for (int k = 0; k < assets::BoneInfluence::k_max_influences; ++k) {
                v.bone_ids[k] = bi.bone_ids[k];
                v.weights[k] = bi.weights[k];
            }
        }
        out.push_back(v);
    }
    return out;
}

// 蒙皮顶点布局：普通 mesh 的 0~4 + 5（bone ids UInt4）+ 6（weights Float4）
inline VertexLayout skinned_vertex_layout() {
    VertexLayout layout;
    layout.stride = sizeof(SkinnedVertexGPU);
    layout.attributes = {
        {0, VertexType::Float3, false, 0},
        {1, VertexType::Float3, false, 3 * sizeof(float)},
        {2, VertexType::Float3, false, 6 * sizeof(float)},
        {3, VertexType::Float2, false, 9 * sizeof(float)},
        {4, VertexType::Float3, false, 11 * sizeof(float)},
        {5, VertexType::UInt4,  false, 14 * sizeof(float)},  // 56
        {6, VertexType::Float4, false, 18 * sizeof(float)}   // 72
    };
    return layout;
}

} // namespace gryce_engine::render
