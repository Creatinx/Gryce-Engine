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
    bool is_complete() const override { return framebuffer_ != VK_NULL_HANDLE; }
    int width() const override { return width_; }
    int height() const override { return height_; }

    // 由 VulkanBackend 调用，开始/结束 shadow render pass
    void begin_render_pass(VkCommandBuffer cmd,
                           VkSubpassContents contents = VK_SUBPASS_CONTENTS_INLINE) const;
    void end_render_pass(VkCommandBuffer cmd) const;

    VkRenderPass render_pass() const { return render_pass_; }
    VkFramebuffer framebuffer() const { return framebuffer_; }
    VulkanTexture* depth_texture() const { return depth_texture_; }

private:
    bool create_render_pass();
    bool create_framebuffer();

    VulkanDevice* device_ = nullptr;
    VulkanSwapchain* swapchain_ = nullptr;

    int width_ = 0;
    int height_ = 0;
    VulkanTexture* color_texture_ = nullptr;
    VulkanTexture* depth_texture_ = nullptr;
    bool owns_depth_texture_ = false;

    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    VkFramebuffer framebuffer_ = VK_NULL_HANDLE;

    bool has_color() const { return color_texture_ != nullptr; }
    bool has_depth() const { return depth_texture_ != nullptr; }
};

} // namespace gryce_engine::render
