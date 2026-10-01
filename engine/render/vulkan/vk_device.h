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

    // 物理设备的 maxPushConstantsSize
    uint32_t max_push_constants_size() const { return max_push_constants_size_; }

    // 全局管线缓存：所有 vkCreateGraphicsPipelines 都应传入它，否则驱动每次
    // 启动都要把 SPIR-V 重新编译成 GPU 机器码（本项目有 70+ 条管线，代价显著）。
    VkPipelineCache pipeline_cache() const { return pipeline_cache_; }

private:
    bool pick_physical_device(VkInstance instance, VkSurfaceKHR surface);
    bool create_logical_device();
    // 从磁盘加载（或新建）管线缓存；save 在 shutdown 时把驱动产物写回磁盘。
    void create_pipeline_cache();
    void save_pipeline_cache();

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
    // allocator_ 保持在类布局末尾（历史位置），避免 ABI 偏移变化导致
    // 其他 TU 中 device_->allocator() 读到损坏值。
    VmaAllocator allocator_ = nullptr;
    uint32_t max_push_constants_size_ = 128;
    // 追加在末尾，避免改动既有成员偏移（见 allocator_ 的 ABI 说明）。
    VkPipelineCache pipeline_cache_ = VK_NULL_HANDLE;
};

} // namespace gryce_engine::render
