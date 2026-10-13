#pragma once

#include "render/framebuffer.h"
#include <vulkan/vulkan.h>

namespace gryce_engine::render {

class VulkanDevice;
class VulkanSwapchain;
class VulkanTexture;

// ---------------------------------------------------------------------------
// VulkanFramebuffer — 当前仅支持 depth-only（shadow map）
// ---------------------------------------------------------------------------
class VulkanFramebuffer : public IFramebuffer {
public:
    VulkanFramebuffer() = default;
    VulkanFramebuffer(VulkanDevice* device, VulkanSwapchain* swapchain);
    ~VulkanFramebuffer() override;

    bool create(int width, int height) override;
    void destroy() override;
    void bind() const override {}
    void unbind() const override {}
    void resize(int width, int height) override;
    void attach_color_texture(ITexture* texture) override;
    void attach_depth_texture(ITexture* texture) override;
    // 解析目标（render pass 的 pResolveAttachments）：多样本颜色在 pass 末尾
    // 自动解析进该单样本纹理，后续 pass 直接采样它。
    void attach_resolve_texture(ITexture* texture) override;

    // 颜色附件 0 的采样数（实现在 .cpp：需要 VulkanTexture 完整定义）。
    int color_sample_count() const override;
    // Vulkan 的 resolve 在 render pass 内由 pResolveAttachments 自动完成，
    // 这里无需（也不能）在 pass 外补做。
    bool resolve_color_to(IFramebuffer* dst) override { (void)dst; return true; }
    bool is_complete() const override { return framebuffer_ != VK_NULL_HANDLE; }
    int width() const override { return width_; }
    int height() const override { return height_; }

    void set_clear_color(float r, float g, float b, float a);

    // 由 VulkanBackend 调用，开始/结束 shadow render pass
    void begin_render_pass(VkCommandBuffer cmd,
                           VkSubpassContents contents = VK_SUBPASS_CONTENTS_INLINE) const;
    void end_render_pass(VkCommandBuffer cmd) const;

    VkRenderPass render_pass() const { return render_pass_; }
    VkFramebuffer framebuffer() const { return framebuffer_; }
    VulkanTexture* depth_texture() const { return depth_texture_; }
    // 关闭 render pass 后回写附件布局缓存时需要（finalLayout 由本类决定）
    VulkanTexture* color_texture() const { return color_texture_; }
    VulkanTexture* resolve_texture() const { return resolve_texture_; }
    // 采样数（int）→ VkSampleCountFlagBits，供 backend / shader 复用同一映射。
    static VkSampleCountFlagBits to_vk_sample_count(int samples);
    VkSampleCountFlagBits color_samples_vk() const {
        return to_vk_sample_count(color_sample_count());
    }

private:
    bool create_render_pass();
    bool create_framebuffer();

    VulkanDevice* device_ = nullptr;
    VulkanSwapchain* swapchain_ = nullptr;

    int width_ = 0;
    int height_ = 0;
    VulkanTexture* color_texture_ = nullptr;
    VulkanTexture* depth_texture_ = nullptr;
    // 多样本颜色的 resolve 目标（单样本）。非空且颜色采样数 > 1 时，
    // render pass 追加 resolve 附件并在 subpass 用 pResolveAttachments 自动解析。
    VulkanTexture* resolve_texture_ = nullptr;
    bool owns_depth_texture_ = false;

    float clear_color_r_ = 0.15f;
    float clear_color_g_ = 0.15f;
    float clear_color_b_ = 0.18f;
    float clear_color_a_ = 1.0f;

    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    VkFramebuffer framebuffer_ = VK_NULL_HANDLE;

    bool has_color() const { return color_texture_ != nullptr; }
    bool has_depth() const { return depth_texture_ != nullptr; }
};

} // namespace gryce_engine::render
