#pragma once
#include "render/rhi_handle.h"
#include "math/math.h"
#include <vector>
#include <cstdint>

namespace gryce_engine::render {

class RenderContext;

struct AtlasSlot {
    int x = 0;
    int y = 0;
    int size = 0;
    bool used = false;
    uint32_t light_id = 0;
};

class ShadowAtlas {
public:
    static constexpr int k_default_atlas_size = 4096;
    static constexpr int k_min_slot_size = 64;

    ShadowAtlas() = default;
    ~ShadowAtlas() { destroy(); }

    bool init(RenderContext* ctx, int atlas_size = k_default_atlas_size);
    void destroy();

    // 分配一个 slot (size x size)，返回 slot 索引，-1 失败
    int allocate(int size, uint32_t light_id);
    void free(int slot_index);
    void free_all();

    // 获取 slot 信息
    const AtlasSlot& get_slot(int index) const { return slots_[index]; }
    int slot_count() const { return (int)slots_.size(); }

    // 纹理访问
    RHITextureHandle atlas_tex() const { return atlas_tex_; }
    RHIFramebufferHandle atlas_fbo() const { return atlas_fbo_; }
    int atlas_size() const { return atlas_size_; }

    // 获取槽位的 UV 变换 (用于 shader 中把 NDC 映射到 atlas 区域)
    // 返回 (offset_x, offset_y, scale_x, scale_y) — 归一化坐标
    math::Vector4f slot_uv_transform(int index) const;

    // 验证: 检查是否有足够空间
    bool has_space(int size) const;

    // 整理: 合并空闲碎片 (简单实现: 重置所有 slot)
    void defragment();

    bool valid() const { return atlas_tex_.is_valid(); }

private:
    RenderContext* ctx_ = nullptr;
    int atlas_size_ = k_default_atlas_size;

    RHITextureHandle atlas_tex_;    // 深度纹理 (Depth24 或 Depth32F)
    RHIFramebufferHandle atlas_fbo_;

    std::vector<AtlasSlot> slots_;

    // 简单贪心分配: 按行排列
    int cursor_x_ = 0;
    int cursor_y_ = 0;
    int row_height_ = 0;

    bool initialized_ = false;
};

} // namespace gryce_engine::render