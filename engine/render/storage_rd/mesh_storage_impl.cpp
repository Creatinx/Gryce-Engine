#include "render/storage_rd/mesh_storage_impl.h"
#include "render/render_context.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::render {

MeshStorageImpl::MeshStorageImpl(RenderContext* ctx)
    : ctx_(ctx) {
}

MeshStorageImpl::~MeshStorageImpl() {
    meshes_.clear();
}

MeshRID MeshStorageImpl::mesh_create() {
    MeshRID rid = next_rid_++;
    meshes_[rid] = MeshData{};
    return rid;
}

void MeshStorageImpl::mesh_free(MeshRID rid) {
    auto it = meshes_.find(rid);
    if (it == meshes_.end()) return;
    meshes_.erase(it);
}

void MeshStorageImpl::mesh_set_vertices(MeshRID rid, const std::vector<MeshVertex>& vertices) {
    auto it = meshes_.find(rid);
    if (it == meshes_.end()) return;
    it->second.vertices = vertices;
    it->second.dirty = true;
}

void MeshStorageImpl::mesh_set_indices(MeshRID rid, const std::vector<uint32_t>& indices) {
    auto it = meshes_.find(rid);
    if (it == meshes_.end()) return;
    it->second.indices = indices;
    it->second.dirty = true;
}

void MeshStorageImpl::mesh_set_submesh_count(MeshRID rid, uint32_t count) {
    auto it = meshes_.find(rid);
    if (it == meshes_.end()) return;
    it->second.submeshes.resize(count);
}

void MeshStorageImpl::mesh_set_submesh(MeshRID rid, uint32_t index, const SubMesh& submesh) {
    auto it = meshes_.find(rid);
    if (it == meshes_.end() || index >= it->second.submeshes.size()) return;
    it->second.submeshes[index] = submesh;
}

uint32_t MeshStorageImpl::mesh_get_vertex_count(MeshRID rid) const {
    auto it = meshes_.find(rid);
    return it != meshes_.end() ? static_cast<uint32_t>(it->second.vertices.size()) : 0;
}

uint32_t MeshStorageImpl::mesh_get_index_count(MeshRID rid) const {
    auto it = meshes_.find(rid);
    return it != meshes_.end() ? static_cast<uint32_t>(it->second.indices.size()) : 0;
}

uint32_t MeshStorageImpl::mesh_get_submesh_count(MeshRID rid) const {
    auto it = meshes_.find(rid);
    return it != meshes_.end() ? static_cast<uint32_t>(it->second.submeshes.size()) : 0;
}

void MeshStorageImpl::update_buffers() {
    // 标记所有脏网格为已清理（GPU 缓冲通过外部渲染管线管理）
    for (auto& [rid, data] : meshes_) {
        if (data.dirty) {
            data.dirty = false;
        }
    }
}

} // namespace gryce_engine::render