#pragma once

#include "render/storage_rd/material_storage.h"
#include <unordered_map>
#include <vector>

namespace gryce_engine::render {

// ---------------------------------------------------------------------------
// MaterialStorageImpl — 材质存储实现
// 管理材质参数、shader 参数绑定、uniform set。
// ---------------------------------------------------------------------------
class MaterialStorageImpl : public RendererMaterialStorage {
public:
    MaterialStorageImpl() = default;
    ~MaterialStorageImpl() override;

    MaterialRID material_create() override;
    void material_free(MaterialRID rid) override;
    void material_set_param(MaterialRID rid, const std::string& name,
                            float value) override;
    void material_set_param(MaterialRID rid, const std::string& name,
                            const math::Vector3f& value) override;
    void material_set_param(MaterialRID rid, const std::string& name,
                            const math::Vector4f& value) override;
    void material_set_texture(MaterialRID rid, const std::string& name,
                              uint32_t texture_rid) override;
    void material_set_shader(MaterialRID rid, uint32_t shader_rid) override;

    void update_buffers() override;

private:
    struct MaterialData {
        std::vector<MaterialParam> params;
        uint32_t shader_rid = 0;
    };

    std::unordered_map<MaterialRID, MaterialData> materials_;
    MaterialRID next_rid_ = 1;
};

} // namespace gryce_engine::render