#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <cstdint>

struct VmaAllocator_T;
using VmaAllocator = VmaAllocator_T*;

namespace gryce_engine::render {

// ---------------------------------------------------------------------------
// VulkanDevice — 物理设备 + 逻辑设备 + 图形队列
// ---------------------------------------------------------------------------
class VulkanDevice {
public:
    bool init(VkInstance instance, VkSurfaceKHR surface);
    void shutdown();

    VkDevice device() const { return device_; }
    VkPhysicalDevice physical_device() const { return physical_device_; }
    VkQueue graphics_queue() const { return graphics_queue_; }
    VkQueue present_queue() const { return present_queue_; }
    uint32_t graphics_queue_family() const { return graphics_family_; }
    uint32_t present_queue_family() const { return present_family_; }
    VmaAllocator allocator() const { return allocator_; }

    bool is_valid() const { return device_ != VK_NULL_HANDLE; }

    // 是否支持 VK_EXT_extended_dynamic_state
    bool supports_extended_dynamic_state() const { return supports_extended_dynamic_state_; }

    // 最大各向异性过滤倍数；若不支持则返回 0
    float max_sampler_anisotropy() const { return max_sampler_anisotropy_; }

private:
    bool pick_physical_device(VkInstance instance, VkSurfaceKHR surface);
    bool create_logical_device();

    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue graphics_queue_ = VK_NULL_HANDLE;
    VkQueue present_queue_ = VK_NULL_HANDLE;
    uint32_t graphics_family_ = UINT32_MAX;
    uint32_t present_family_ = UINT32_MAX;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;

    bool supports_extended_dynamic_state_ = false;
    bool supports_anisotropy_ = false;
    float max_sampler_anisotropy_ = 0.0f;
    VmaAllocator allocator_ = nullptr;
};

} // namespace gryce_engine::render
