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
    bool create_empty(int width, int height, int channels = 4) override;
    bool upload_data(const void* data, int width, int height, int channels = 4) override;
    bool create_depth(int width, int height) override;
    bool create(TextureFormat format, int width, int height, const void* data = nullptr) override;

    void bind(uint32_t slot = 0) const override;
    void unbind() const override;

    void set_filter(TextureFilter min, TextureFilter mag) override;
    void set_wrap(TextureWrap s, TextureWrap t) override;

    int width() const override { return width_; }
    int height() const override { return height_; }
    bool is_valid() const override { return image_ != VK_NULL_HANDLE; }

    VkImageView image_view() const { return image_view_; }
    VkSampler sampler() const { return sampler_; }
    VkImageLayout layout() const { return layout_; }
    VkFormat format() const { return format_; }
    bool is_depth() const;

    void transition_layout(VkCommandBuffer cmd, VkImageLayout new_layout);
    void set_layout(VkImageLayout layout) { layout_ = layout; }

private:
    void destroy();
    bool create_image(VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                      const void* data = nullptr);
    bool upload_with_staging(const void* data, VkDeviceSize size);

    VulkanDevice* device_ = nullptr;
    VkImage image_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = nullptr;
    VkImageView image_view_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkImageLayout layout_ = VK_IMAGE_LAYOUT_UNDEFINED;

    int width_ = 0;
    int height_ = 0;
    int channels_ = 4;
    VkFormat format_ = VK_FORMAT_R8G8B8A8_UNORM;
    TextureFilter min_filter_ = TextureFilter::Linear;
    TextureFilter mag_filter_ = TextureFilter::Linear;
    TextureWrap wrap_s_ = TextureWrap::Repeat;
    TextureWrap wrap_t_ = TextureWrap::Repeat;
};

} // namespace gryce_engine::render
