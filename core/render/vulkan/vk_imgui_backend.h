#pragma once

#include <vulkan/vulkan.h>

#include <list>
#include <unordered_map>

#include "render/imgui_backend.h"

namespace gryce_engine::render {

// ---------------------------------------------------------------------------
// VulkanImGuiBackend — 使用 imgui_impl_vulkan 绘制 ImGui
// ---------------------------------------------------------------------------
class VulkanBackend;

class VulkanImGuiBackend : public IImGuiBackend {
public:
    explicit VulkanImGuiBackend(class IRenderBackend* backend);
    ~VulkanImGuiBackend() override;

    bool init() override;
    void shutdown() override;
    void new_frame() override;
    void render_draw_data(ImDrawData* draw_data) override;
    bool is_vulkan() const override { return true; }

    // Vulkan 端 ImTextureID 即 VkDescriptorSet；为纹理创建/缓存 descriptor set。
    uint64_t imgui_texture_id(ITexture* texture) const override;

    // 重新上传字体 atlas 到 GPU（运行时字体大小热重载）。
    void rebuild_fonts() override;

    // 纹理销毁时由 VulkanBackend 调用，移除缓存的 descriptor set。
    void invalidate_texture(ITexture* tex);

private:
    void clear_cache();

    VulkanBackend* backend_ = nullptr;
    bool initialized_ = false;

    // 纹理指针 -> descriptor set 缓存。
    // Vulkan 创建 descriptor set 有开销，且需要显式 RemoveTexture 释放；
    // 缓存大小受限，纹理销毁时通过 invalidate_texture 移除，避免池槽位复用后误判。
    struct CacheEntry {
        ITexture* texture = nullptr;
        VkDescriptorSet set = VK_NULL_HANDLE;
    };
    mutable std::list<CacheEntry> cache_lru_;
    mutable std::unordered_map<ITexture*, std::list<CacheEntry>::iterator> cache_map_;
    static constexpr size_t k_max_cache_size = 8;
};

} // namespace gryce_engine::render
