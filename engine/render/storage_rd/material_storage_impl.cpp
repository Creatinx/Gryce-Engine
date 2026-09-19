#include "render/storage_rd/material_storage_impl.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::render {

MaterialStorageImpl::~MaterialStorageImpl() {
    materials_.clear();
}

MaterialRID MaterialStorageImpl::material_create() {
    MaterialRID rid = next_rid_++;
    materials_[rid] = MaterialData{};
    return rid;
}

void MaterialStorageImpl::material_free(MaterialRID rid) {
    materials_.erase(rid);
}

void MaterialStorageImpl::material_set_param(MaterialRID rid, const std::string& name,
                                              float value) {
    auto it = materials_.find(rid);
    if (it == materials_.end()) return;
    MaterialParam p;
    p.name = name;
    p.type = MaterialParamType::Float;
    p.float_value = value;
    it->second.params.push_back(p);
}

void MaterialStorageImpl::material_set_param(MaterialRID rid, const std::string& name,
                                              const math::Vector3f& value) {
    auto it = materials_.find(rid);
    if (it == materials_.end()) return;
    MaterialParam p;
    p.name = name;
    p.type = MaterialParamType::Vec3;
    p.vec4_value = math::Vector4f(value.x, value.y, value.z, 0.0f);
    it->second.params.push_back(p);
}

void MaterialStorageImpl::material_set_param(MaterialRID rid, const std::string& name,
                                              const math::Vector4f& value) {
    auto it = materials_.find(rid);
    if (it == materials_.end()) return;
    MaterialParam p;
    p.name = name;
    p.type = MaterialParamType::Vec4;
    p.vec4_value = value;
    it->second.params.push_back(p);
}

void MaterialStorageImpl::material_set_texture(MaterialRID rid, const std::string& name,
                                                uint32_t texture_rid) {
    auto it = materials_.find(rid);
    if (it == materials_.end()) return;
    MaterialParam p;
    p.name = name;
    p.type = MaterialParamType::Texture;
    p.texture_rid = texture_rid;
    it->second.params.push_back(p);
}

void MaterialStorageImpl::material_set_shader(MaterialRID rid, uint32_t shader_rid) {
    auto it = materials_.find(rid);
    if (it == materials_.end()) return;
    it->second.shader_rid = shader_rid;
}

void MaterialStorageImpl::update_buffers() {
    // 材质不需要 GPU 缓冲同步，参数在渲染时通过 uniform 设置
}

} // namespace gryce_engine::render