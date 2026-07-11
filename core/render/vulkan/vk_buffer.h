#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>

struct VmaAllocation_T;
using VmaAllocation = VmaAllocation_T*;

namespace gryce_engine::render {

class VulkanDevice;

// ---------------------------------------------------------------------------
// VulkanBuffer — GPU 缓冲助手（顶点/索引/暂存）
// ---------------------------------------------------------------------------
class VulkanBuffer {
public:
    VulkanBuffer() = default;
    ~VulkanBuffer();

    VulkanBuffer(const VulkanBuffer&) = delete;
    VulkanBuffer& operator=(const VulkanBuffer&) = delete;

    VulkanBuffer(VulkanBuffer&& other) noexcept;
    VulkanBuffer& operator=(VulkanBuffer&& other) noexcept;

    bool init(VulkanDevice* device, VkDeviceSize size, VkBufferUsageFlags usage,
              VkMemoryPropertyFlags properties);
    void shutdown();

    bool upload(const void* data, VkDeviceSize size, VkDeviceSize offset = 0) const;

    VkBuffer buffer() const { return buffer_; }
    VkDeviceSize size() const { return size_; }
    void* mapped() const { return mapped_; }

private:
    VulkanDevice* device_ = nullptr;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = nullptr;
    VkDeviceSize size_ = 0;
    VkMemoryPropertyFlags properties_ = 0;
    void* mapped_ = nullptr;
    bool is_coherent_ = false;
};

} // namespace gryce_engine::render
