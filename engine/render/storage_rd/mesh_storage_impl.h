#pragma once

#include "render/storage_rd/mesh_storage.h"
#include "render/rhi_handle.h"
#include <unordered_map>
#include <vector>

namespace gryce_engine::render {

class RenderContext;

// ---------------------------------------------------------------------------
// MeshStorageImpl — 网格存储实现
// 管理网格的顶点缓冲、索引缓冲、子网格数据。
// 使用 RID 分配 + GPU 缓冲同步。
// ---------------------------------------------------------------------------
class MeshStorageImpl : public RendererMeshStorage {
public:
    MeshStorageImpl(RenderContext* ctx);
    ~MeshStorageImpl() override;

    MeshRID mesh_create() override;
    void mesh_free(MeshRID rid) override;
    void mesh_set_vertices(MeshRID rid, const std::vector<MeshVertex>& vertices) override;
    void mesh_set_indices(MeshRID rid, const std::vector<uint32_t>& indices) override;
    void mesh_set_submesh_count(MeshRID rid, uint32_t count) override;
    void mesh_set_submesh(MeshRID rid, uint32_t index, const SubMesh& submesh) override;

    uint32_t mesh_get_vertex_count(MeshRID rid) const override;
    uint32_t mesh_get_index_count(MeshRID rid) const override;
    uint32_t mesh_get_submesh_count(MeshRID rid) const override;

    void update_buffers() override;

private:
    struct MeshData {
        std::vector<MeshVertex> vertices;
        std::vector<uint32_t> indices;
        std::vector<SubMesh> submeshes;
        RHIBufferHandle vertex_buffer;
        RHIBufferHandle index_buffer;
        bool dirty = true;
    };

    RenderContext* ctx_ = nullptr;
    std::unordered_map<MeshRID, MeshData> meshes_;
    MeshRID next_rid_ = 1;
};

} // namespace gryce_engine::render