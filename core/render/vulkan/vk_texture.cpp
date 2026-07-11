#include "vk_texture.h"

#include "vk_buffer.h"
#include "vk_device.h"
#include "utils/glog/glog_lib.h"

#include <vma/vk_mem_alloc.h>
#include <stb_image.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace gryce_engine::render {

namespace {

VkFormat channels_to_format(int channels, bool srgb) {
    switch (channels) {
        case 1: return VK_FORMAT_R8_UNORM;
        case 2: return VK_FORMAT_R8G8_UNORM;
        case 3: return srgb ? VK_FORMAT_R8G8B8_SRGB : VK_FORMAT_R8G8B8_UNORM;
        case 4: return srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    }
    return VK_FORMAT_R8G8B8A8_UNORM;
}

VkFilter to_vk_filter(TextureFilter f) {
    switch (f) {
        case TextureFilter::Nearest:
        case TextureFilter::NearestMipmapNearest:
            return VK_FILTER_NEAREST;
        case TextureFilter::Linear:
        case TextureFilter::LinearMipmapLinear:
            return VK_FILTER_LINEAR;
    }
    return VK_FILTER_LINEAR;
}

VkSamplerAddressMode to_vk_wrap(TextureWrap w) {
    switch (w) {
        case TextureWrap::Repeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case TextureWrap::ClampToEdge: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case TextureWrap::ClampToBorder: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    }
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

VkCommandBuffer begin_one_time_commands(VulkanDevice* device, VkCommandPool pool) {
    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device->device(), &alloc, &cmd);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    return cmd;
}

void end_one_time_commands(VulkanDevice* device, VkCommandPool pool, VkCommandBuffer cmd) {
    vkEndCommandBuffer(cmd);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(device->graphics_queue(), 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(device->graphics_queue());
    vkFreeCommandBuffers(device->device(), pool, 1, &cmd);
}

} // namespace

VulkanTexture::VulkanTexture(VulkanDevice* device) : device_(device) {}

VulkanTexture::~VulkanTexture() {
    destroy();
}

void VulkanTexture::destroy() {
    if (!device_ || !device_->is_valid()) return;
    VkDevice dev = device_->device();
    if (sampler_) vkDestroySampler(dev, sampler_, nullptr);
    if (image_view_) vkDestroyImageView(dev, image_view_, nullptr);
    if (image_) vmaDestroyImage(device_->allocator(), image_, allocation_);
    sampler_ = VK_NULL_HANDLE;
    image_view_ = VK_NULL_HANDLE;
    image_ = VK_NULL_HANDLE;
    allocation_ = nullptr;
}

bool VulkanTexture::load_from_file(const std::string& path) {
    int w = 0, h = 0, ch = 0;
    stbi_uc* pixels = stbi_load(path.c_str(), &w, &h, &ch, STBI_rgb_alpha);
    if (!pixels) {
        GLOG_ERROR("VulkanTexture: failed to load image '{}'", path);
        return false;
    }
    bool ok = upload_data(pixels, w, h, 4);
    stbi_image_free(pixels);
    return ok;
}

bool VulkanTexture::create_empty(int width, int height, int channels) {
    return upload_data(nullptr, width, height, channels);
}

bool VulkanTexture::upload_data(const void* data, int width, int height, int channels) {
    destroy();
    width_ = width;
    height_ = height;

    // 单通道 R8 在某些 GPU/驱动组合下会出现采样为 0 或采样异常的问题，
    // 统一扩展成 RGBA8 更稳定；shader 仍采样 .r 通道，与 OpenGL 行为保持一致。
    std::vector<unsigned char> converted;
    const void* upload_data = data;
    if (channels == 1 && data) {
        channels_ = 4;
        converted.resize(static_cast<std::size_t>(width) * height * 4);
        const unsigned char* src = static_cast<const unsigned char*>(data);
        unsigned char* dst = converted.data();
        for (int i = 0; i < width * height; ++i) {
            unsigned char v = src[i];
            dst[i * 4 + 0] = v;
            dst[i * 4 + 1] = v;
            dst[i * 4 + 2] = v;
            dst[i * 4 + 3] = v;
        }
        upload_data = converted.data();
    } else if (channels == 3 && data) {
        channels_ = 4;
        converted.resize(static_cast<std::size_t>(width) * height * 4);
        const unsigned char* src = static_cast<const unsigned char*>(data);
        unsigned char* dst = converted.data();
        for (int i = 0; i < width * height; ++i) {
            dst[i * 4 + 0] = src[i * 3 + 0];
            dst[i * 4 + 1] = src[i * 3 + 1];
            dst[i * 4 + 2] = src[i * 3 + 2];
            dst[i * 4 + 3] = 255;
        }
        upload_data = converted.data();
    } else {
        channels_ = channels;
    }
    format_ = channels_to_format(channels_, false);

    return create_image(format_, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT, upload_data);
}

bool VulkanTexture::create_depth(int width, int height) {
    destroy();
    width_ = width;
    height_ = height;
    channels_ = 1;
    format_ = VK_FORMAT_D32_SFLOAT;
    return create_image(VK_FORMAT_D32_SFLOAT,
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        VK_IMAGE_ASPECT_DEPTH_BIT, nullptr);
}

bool VulkanTexture::create(TextureFormat format, int width, int height, const void* data) {
    destroy();
    width_ = width;
    height_ = height;
    VkFormat vk_format = VK_FORMAT_R8G8B8A8_UNORM;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    switch (format) {
        case TextureFormat::RGB8: vk_format = VK_FORMAT_R8G8B8_UNORM; break;
        case TextureFormat::RGBA8: vk_format = VK_FORMAT_R8G8B8A8_UNORM; break;
        case TextureFormat::RGBA16F: vk_format = VK_FORMAT_R16G16B16A16_SFLOAT; usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT; break;
        case TextureFormat::R8: vk_format = VK_FORMAT_R8_UNORM; break;
        case TextureFormat::Depth16: vk_format = VK_FORMAT_D16_UNORM; usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT; aspect = VK_IMAGE_ASPECT_DEPTH_BIT; break;
        case TextureFormat::Depth24: vk_format = VK_FORMAT_X8_D24_UNORM_PACK32; usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT; aspect = VK_IMAGE_ASPECT_DEPTH_BIT; break;
        case TextureFormat::Depth32F: vk_format = VK_FORMAT_D32_SFLOAT; usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT; aspect = VK_IMAGE_ASPECT_DEPTH_BIT; break;
        case TextureFormat::Depth24Stencil8: vk_format = VK_FORMAT_D24_UNORM_S8_UINT; usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT; aspect = VK_IMAGE_ASPECT_DEPTH_BIT; break;
        default: break;
    }
    format_ = vk_format;
    return create_image(vk_format, usage, aspect, data);
}

bool VulkanTexture::create_image(VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                                 const void* data) {
    VkDevice dev = device_->device();

    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.extent.width = static_cast<uint32_t>(width_);
    info.extent.height = static_cast<uint32_t>(height_);
    info.extent.depth = 1;
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.format = format;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    info.usage = usage;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo alloc_info{};
    alloc_info.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    if (vmaCreateImage(device_->allocator(), &info, &alloc_info, &image_, &allocation_, nullptr) != VK_SUCCESS) {
        GLOG_ERROR("VulkanTexture: failed to create image");
        return false;
    }

    // image view
    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = image_;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange.aspectMask = aspect;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = 1;
    if (vkCreateImageView(dev, &view_info, nullptr, &image_view_) != VK_SUCCESS) {
        GLOG_ERROR("VulkanTexture: failed to create image view");
        return false;
    }

    // sampler
    const bool is_depth = (aspect == VK_IMAGE_ASPECT_DEPTH_BIT);
    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = to_vk_filter(mag_filter_);
    sampler_info.minFilter = to_vk_filter(min_filter_);
    sampler_info.addressModeU = to_vk_wrap(wrap_s_);
    sampler_info.addressModeV = to_vk_wrap(wrap_t_);
    sampler_info.addressModeW = to_vk_wrap(wrap_s_);
    sampler_info.anisotropyEnable = VK_FALSE;
    sampler_info.maxAnisotropy = 1.0f;
    sampler_info.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    sampler_info.unnormalizedCoordinates = VK_FALSE;
    sampler_info.compareEnable = is_depth ? VK_TRUE : VK_FALSE;
    sampler_info.compareOp = is_depth ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_NEVER;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_info.maxLod = 1.0f;
    if (vkCreateSampler(dev, &sampler_info, nullptr, &sampler_) != VK_SUCCESS) {
        GLOG_ERROR("VulkanTexture: failed to create sampler");
        return false;
    }

    // upload data if provided
    if (data && (usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        VkDeviceSize size = static_cast<VkDeviceSize>(width_ * height_ * channels_);
        return upload_with_staging(data, size);
    }

    layout_ = (aspect == VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                                    : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return true;
}

bool VulkanTexture::upload_with_staging(const void* data, VkDeviceSize size) {
    VulkanBuffer staging;
    if (!staging.init(device_, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        return false;
    }
    staging.upload(data, size);

    // 需要临时 command pool：复用 VulkanSwapchain 的 pool 较麻烦，这里单独建一个
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = device_->graphics_queue_family();
    VkCommandPool pool = VK_NULL_HANDLE;
    vkCreateCommandPool(device_->device(), &pool_info, nullptr, &pool);

    VkCommandBuffer cmd = begin_one_time_commands(device_, pool);

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image_;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {static_cast<uint32_t>(width_), static_cast<uint32_t>(height_), 1};
    vkCmdCopyBufferToImage(cmd, staging.buffer(), image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    end_one_time_commands(device_, pool, cmd);

    vkDestroyCommandPool(device_->device(), pool, nullptr);

    layout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return true;
}

void VulkanTexture::bind(uint32_t /*slot*/) const {
    // Vulkan 绑定通过 descriptor set，这里不做操作
}

void VulkanTexture::unbind() const {}

bool VulkanTexture::is_depth() const {
    switch (format_) {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_X8_D24_UNORM_PACK32:
        case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            return true;
        default:
            return false;
    }
}

void VulkanTexture::transition_layout(VkCommandBuffer cmd, VkImageLayout new_layout) {
    if (new_layout == layout_) return;

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = layout_;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image_;
    barrier.subresourceRange.aspectMask = is_depth() ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkPipelineStageFlags dst_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;

    switch (layout_) {
        case VK_IMAGE_LAYOUT_UNDEFINED:
            barrier.srcAccessMask = 0;
            src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            break;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
            barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            src_stage = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            break;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
            barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            src_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            break;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            src_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            break;
        default:
            break;
    }

    switch (new_layout) {
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
            barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            dst_stage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
            break;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            dst_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            break;
        default:
            break;
    }

    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    layout_ = new_layout;
}

void VulkanTexture::set_filter(TextureFilter min, TextureFilter mag) {
    min_filter_ = min;
    mag_filter_ = mag;
    if (sampler_) {
        // 重新创建 sampler
        const bool is_depth = (format_ == VK_FORMAT_D32_SFLOAT ||
                               format_ == VK_FORMAT_D16_UNORM ||
                               format_ == VK_FORMAT_D24_UNORM_S8_UINT ||
                               format_ == VK_FORMAT_X8_D24_UNORM_PACK32);
        VkDevice dev = device_->device();
        vkDestroySampler(dev, sampler_, nullptr);
        VkSamplerCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.magFilter = to_vk_filter(mag_filter_);
        info.minFilter = to_vk_filter(min_filter_);
        info.addressModeU = to_vk_wrap(wrap_s_);
        info.addressModeV = to_vk_wrap(wrap_t_);
        info.addressModeW = to_vk_wrap(wrap_s_);
        info.anisotropyEnable = VK_FALSE;
        info.maxAnisotropy = 1.0f;
        info.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
        info.unnormalizedCoordinates = VK_FALSE;
        info.compareEnable = is_depth ? VK_TRUE : VK_FALSE;
        info.compareOp = is_depth ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_NEVER;
        info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        info.maxLod = 1.0f;
        vkCreateSampler(dev, &info, nullptr, &sampler_);
    }
}

void VulkanTexture::set_wrap(TextureWrap s, TextureWrap t) {
    wrap_s_ = s;
    wrap_t_ = t;
    set_filter(min_filter_, mag_filter_);
}

} // namespace gryce_engine::render
