#pragma once

#include "render/texture.h"

#include <vulkan/vulkan.h>

struct VmaAllocation_T;
using VmaAllocation = VmaAllocation_T*;

namespace gryce_engine::render {

class VulkanDevice;

// ---------------------------------------------------------------------------
// VulkanTexture — image / view / sampler
// ---------------------------------------------------------------------------
class VulkanTexture : public ITexture {
public:
    VulkanTexture() = default;
    explicit VulkanTexture(VulkanDevice* device);
    ~VulkanTexture() override;

    bool load_from_file(const std::string& path) override;
    bool load_from_memory(const void* data, size_t size) override;
    bool create_empty(int width, int height, int channels = 4) override;
    bool upload_data(const void* data, int width, int height, int channels = 4) override;
    bool upload_cubemap(const void* faces[6], int width, int height, int channels = 4) override;
    bool upload_cubemap_hdr(const void* faces[6], int width, int height) override;
    bool upload_cubemap_hdr_mips(const void* const* mip_faces, int mip_levels,
                                 int width, int height) override;
    bool is_cubemap() const override { return is_cubemap_; }
    bool create_depth(int width, int height) override;
    bool create(TextureFormat format, int width, int height, const void* data = nullptr) override;
    bool create_compressed(TextureFormat format, int width, int height,
                           int mip_levels, const void* const* mip_data,
                           const size_t* mip_sizes) override;

    void bind(uint32_t slot = 0) const override;
    void unbind() const override;

    void set_filter(TextureFilter min, TextureFilter mag) override;
    void set_wrap(TextureWrap s, TextureWrap t) override;

    int width() const override { return width_; }
    int height() const override { return height_; }
    bool is_valid() const override { return image_ != VK_NULL_HANDLE; }

    VkImageView image_view() const { return image_view_; }
    VkSampler sampler() const { return sampler_; }
    // 非比较 sampler（仅深度贴图）：PCSS 需要用 sampler2D 读原始深度
    VkSampler depth_sampler() const { return depth_sampler_; }
    VkImageLayout layout() const { return layout_; }
    VkFormat format() const { return format_; }
    bool is_depth() const;

    void transition_layout(VkCommandBuffer cmd, VkImageLayout new_layout);
    void set_layout(VkImageLayout layout) { layout_ = layout; }

private:
    void destroy();
    bool create_image(VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                      const void* data = nullptr);
    // 创建后立即把带 SAMPLED 用途的渲染目标过渡到可采样布局，避免
    // 首次作为附件渲染前被采样时 layout 仍是 UNDEFINED（验证层报错 + 垃圾色）
    void initial_transition_to_read_only(VkImageAspectFlags aspect);
    bool upload_with_staging(const void* data, VkDeviceSize size);

    VulkanDevice* device_ = nullptr;
    VkImage image_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = nullptr;
    VkImageView image_view_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkSampler depth_sampler_ = VK_NULL_HANDLE;
    VkImageLayout layout_ = VK_IMAGE_LAYOUT_UNDEFINED;

    int width_ = 0;
    int height_ = 0;
    int channels_ = 4;
    bool is_cubemap_ = false;
    uint32_t mip_levels_ = 1;
    VkFormat format_ = VK_FORMAT_R8G8B8A8_UNORM;
    TextureFilter min_filter_ = TextureFilter::Linear;
    TextureFilter mag_filter_ = TextureFilter::Linear;
    TextureWrap wrap_s_ = TextureWrap::Repeat;
    TextureWrap wrap_t_ = TextureWrap::Repeat;
};

} // namespace gryce_engine::render
