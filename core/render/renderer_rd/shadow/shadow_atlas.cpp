#include "render/renderer_rd/shadow/shadow_atlas.h"
#include "render/render_context.h"
#include "render/texture.h"
#include "render/framebuffer.h"
#include "utils/glog/glog_lib.h"

#include <algorithm>

namespace gryce_engine::render {

// ---------------------------------------------------------------------------
// init: 创建 atlas_size_ x atlas_size_ 的 Depth24 纹理 + FBO
// ---------------------------------------------------------------------------
bool ShadowAtlas::init(RenderContext* ctx, int atlas_size) {
    if (initialized_) return true;
    ctx_ = ctx;
    atlas_size_ = atlas_size;

    // 创建深度纹理
    atlas_tex_ = ctx_->create_texture();
    ITexture* tex = ctx_->texture(atlas_tex_);
    if (!atlas_tex_.is_valid() || !tex ||
        !tex->create_depth(atlas_size_, atlas_size_)) {
        GLOG_ERROR("ShadowAtlas: failed to create depth texture ({}x{})", atlas_size_, atlas_size_);
        return false;
    }
    tex->set_filter(TextureFilter::Linear, TextureFilter::Linear);
    tex->set_wrap(TextureWrap::ClampToBorder, TextureWrap::ClampToBorder);

    // 创建 FBO
    atlas_fbo_ = ctx_->create_framebuffer();
    IFramebuffer* fbo = ctx_->framebuffer(atlas_fbo_);
    if (!atlas_fbo_.is_valid() || !fbo || !fbo->create(atlas_size_, atlas_size_)) {
        GLOG_ERROR("ShadowAtlas: failed to create FBO");
        ctx_->destroy_texture(atlas_tex_);
        atlas_tex_ = {};
        return false;
    }
    fbo->attach_depth_texture(tex);
    if (!fbo->is_complete()) {
        GLOG_ERROR("ShadowAtlas: FBO incomplete");
        ctx_->destroy_texture(atlas_tex_);
        ctx_->destroy_framebuffer(atlas_fbo_);
        atlas_tex_ = {};
        atlas_fbo_ = {};
        return false;
    }

    // 初始化光标
    cursor_x_ = 0;
    cursor_y_ = 0;
    row_height_ = 0;
    slots_.clear();

    initialized_ = true;
    GLOG_INFO("ShadowAtlas initialized ({}x{})", atlas_size_, atlas_size_);
    return true;
}

// ---------------------------------------------------------------------------
// destroy: 释放资源
// ---------------------------------------------------------------------------
void ShadowAtlas::destroy() {
    if (!initialized_) return;

    if (atlas_fbo_.is_valid()) {
        ctx_->destroy_framebuffer(atlas_fbo_);
        atlas_fbo_ = {};
    }
    if (atlas_tex_.is_valid()) {
        ctx_->destroy_texture(atlas_tex_);
        atlas_tex_ = {};
    }

    slots_.clear();
    cursor_x_ = 0;
    cursor_y_ = 0;
    row_height_ = 0;
    initialized_ = false;
}

// ---------------------------------------------------------------------------
// allocate: 使用简单行包装算法分配 slot
// 类似 sprite 图集打包: 按行从左到右排列，当前行不够则换行
// ---------------------------------------------------------------------------
int ShadowAtlas::allocate(int size, uint32_t light_id) {
    if (!initialized_) return -1;
    if (size < k_min_slot_size) size = k_min_slot_size;

    // 如果当前行剩余宽度不够，换行
    if (cursor_x_ + size > atlas_size_) {
        cursor_x_ = 0;
        cursor_y_ += row_height_;
        row_height_ = 0;
    }

    // 检查总高度是否足够
    if (cursor_y_ + size > atlas_size_) {
        GLOG_WARN("ShadowAtlas: no space left for {}x{} slot", size, size);
        return -1;
    }

    // 创建新 slot
    AtlasSlot slot;
    slot.x = cursor_x_;
    slot.y = cursor_y_;
    slot.size = size;
    slot.used = true;
    slot.light_id = light_id;

    int index = (int)slots_.size();
    slots_.push_back(slot);

    // 更新光标
    cursor_x_ += size;
    row_height_ = std::max(row_height_, size);

    return index;
}

// ---------------------------------------------------------------------------
// free: 标记 slot 为未使用
// ---------------------------------------------------------------------------
void ShadowAtlas::free(int slot_index) {
    if (slot_index < 0 || slot_index >= (int)slots_.size()) return;
    slots_[slot_index].used = false;
}

// ---------------------------------------------------------------------------
// free_all: 清空所有 slot
// ---------------------------------------------------------------------------
void ShadowAtlas::free_all() {
    // 重置所有 slot 为未使用状态，但保留 slot 数组（避免重新分配）
    for (auto& slot : slots_) {
        slot.used = false;
    }
    // 重置光标位置
    cursor_x_ = 0;
    cursor_y_ = 0;
    row_height_ = 0;
}

// ---------------------------------------------------------------------------
// slot_uv_transform: 返回槽位的归一化 UV 变换 (offset_x, offset_y, scale_x, scale_y)
// 用于 shader 中把 NDC 坐标映射到 atlas 的对应区域
// ---------------------------------------------------------------------------
math::Vector4f ShadowAtlas::slot_uv_transform(int index) const {
    if (index < 0 || index >= (int)slots_.size()) {
        return math::Vector4f(0.0f, 0.0f, 1.0f, 1.0f);
    }
    const AtlasSlot& slot = slots_[index];
    float inv_size = 1.0f / (float)atlas_size_;
    return math::Vector4f(
        (float)slot.x * inv_size,   // offset_x
        (float)slot.y * inv_size,   // offset_y
        (float)slot.size * inv_size, // scale_x
        (float)slot.size * inv_size  // scale_y
    );
}

// ---------------------------------------------------------------------------
// has_space: 检查是否有足够空间分配 size x size 的 slot
// ---------------------------------------------------------------------------
bool ShadowAtlas::has_space(int size) const {
    if (!initialized_) return false;
    if (size < k_min_slot_size) size = k_min_slot_size;

    // 如果当前行剩余宽度够，检查高度
    if (cursor_x_ + size <= atlas_size_) {
        return cursor_y_ + std::max(row_height_, size) <= atlas_size_;
    }

    // 换行后检查高度
    return cursor_y_ + row_height_ + size <= atlas_size_;
}

// ---------------------------------------------------------------------------
// defragment: 重置光标位置，标记所有 slot 为空
// ---------------------------------------------------------------------------
void ShadowAtlas::defragment() {
    free_all();
    slots_.clear();
}

} // namespace gryce_engine::render