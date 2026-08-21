#pragma once

#include <cstdint>
#include <vector>

#include "render/export.h"
#include "math/math.h"

namespace gryce_engine::render {

// 网格 RID 句柄
using MeshRID = uint32_t;
constexpr MeshRID k_invalid_mesh_rid = 0;

// 顶点格式
struct MeshVertex {
    math::Vector3f position;
    math::Vector3f normal;
    math::Vector3f tangent;
    math::Vector2f uv;
    math::Vector2f uv2;
    math::Vector4f color = math::Vector4f(1, 1, 1, 1);
};

// 子网格
struct SubMesh {
    uint32_t index_start = 0;
    uint32_t index_count = 0;
    uint32_t vertex_start = 0;
    uint32_t vertex_count = 0;
    uint32_t material_id = 0;
};

// ---------------------------------------------------------------------------
// RendererMeshStorage — 网格存储抽象接口
// 管理网格的顶点缓冲、索引缓冲、子网格数据。
// ---------------------------------------------------------------------------
class GRYCE_RENDERER_API RendererMeshStorage {
public:
    virtual ~RendererMeshStorage() = default;

    virtual MeshRID mesh_create() = 0;
    virtual void mesh_free(MeshRID rid) = 0;
    virtual void mesh_set_vertices(MeshRID rid, const std::vector<MeshVertex>& vertices) = 0;
    virtual void mesh_set_indices(MeshRID rid, const std::vector<uint32_t>& indices) = 0;
    virtual void mesh_set_submesh_count(MeshRID rid, uint32_t count) = 0;
    virtual void mesh_set_submesh(MeshRID rid, uint32_t index, const SubMesh& submesh) = 0;

    virtual uint32_t mesh_get_vertex_count(MeshRID rid) const = 0;
    virtual uint32_t mesh_get_index_count(MeshRID rid) const = 0;
    virtual uint32_t mesh_get_submesh_count(MeshRID rid) const = 0;

    virtual void update_buffers() = 0;
};

} // namespace gryce_engine::render