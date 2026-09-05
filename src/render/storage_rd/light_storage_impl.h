#pragma once

#include "render/storage_rd/light_storage.h"
#include "render/rhi_handle.h"
#include <unordered_map>
#include <vector>

namespace gryce_engine::render {

class RenderContext;

// ---------------------------------------------------------------------------
// LightStorageImpl — 光源存储实现
// 管理所有光源的创建、更新、销毁，维护光源 GPU 数据缓冲（SSBO）。
// ---------------------------------------------------------------------------
class LightStorageImpl : public RendererLightStorage {
public:
    static constexpr int k_max_lights = 256;
    // 每个光源在 SSBO 中占 20 个 float（80 字节）
    // float4 type_padding, float4 position, float4 direction,
    // float4 color_intensity, float4 params
    static constexpr int k_floats_per_light = 20;
    static constexpr size_t k_ssbo_size = static_cast<size_t>(k_max_lights) * k_floats_per_light * sizeof(float);

    LightStorageImpl(RenderContext* ctx);
    ~LightStorageImpl() override;

    LightRID light_create(LightType type) override;
    void light_free(LightRID rid) override;
    void light_set_color(LightRID rid, const math::Vector3f& color) override;
    void light_set_intensity(LightRID rid, float intensity) override;
    void light_set_position(LightRID rid, const math::Vector3f& pos) override;
    void light_set_direction(LightRID rid, const math::Vector3f& dir) override;
    void light_set_range(LightRID rid, float range) override;
    void light_set_spot_angle(LightRID rid, float angle) override;
    void light_set_shadow_enabled(LightRID rid, bool enabled) override;

    const LightData* get_light_data(LightRID rid) const override;
    size_t light_count() const override;
    void update_buffers() override;

    // 绑定 SSBO 到指定 binding point（供渲染管线使用）
    void bind_light_buffer(uint32_t binding_point = 0);

    // 获取 SSBO 句柄
    RHIBufferHandle light_buffer() const { return light_buffer_; }

private:
    // 打包光源数据到 GPU 缓冲区格式
    void _pack_lights(std::vector<float>& out_buffer) const;

    RenderContext* ctx_ = nullptr;
    std::unordered_map<LightRID, LightData> lights_;
    LightRID next_rid_ = 1;

    // GPU SSBO 句柄
    RHIBufferHandle light_buffer_;
    bool buffer_dirty_ = true;
};

} // namespace gryce_engine::render