#include "vk_buffer.h"

#include "vk_device.h"
#include "utils/glog/glog_lib.h"

#include <vma/vk_mem_alloc.h>

#include <cstring>

namespace gryce_engine::render {

VulkanBuffer::~VulkanBuffer() {
    shutdown();
}

VulkanBuffer::VulkanBuffer(VulkanBuffer&& other) noexcept
    : device_(other.device_)
    , buffer_(other.buffer_)
    , allocation_(other.allocation_)
    , size_(other.size_)
    , properties_(other.properties_)
    , mapped_(other.mapped_)
    , is_coherent_(other.is_coherent_) {
    other.device_ = nullptr;
    other.buffer_ = VK_NULL_HANDLE;
    other.allocation_ = nullptr;
    other.mapped_ = nullptr;
}

VulkanBuffer& VulkanBuffer::operator=(VulkanBuffer&& other) noexcept {
    if (this != &other) {
        shutdown();
        device_ = other.device_;
        buffer_ = other.buffer_;
        allocation_ = other.allocation_;
        size_ = other.size_;
        properties_ = other.properties_;
        mapped_ = other.mapped_;
        is_coherent_ = other.is_coherent_;
        other.device_ = nullptr;
        other.buffer_ = VK_NULL_HANDLE;
        other.allocation_ = nullptr;
        other.mapped_ = nullptr;
    }
    return *this;
}

bool VulkanBuffer::init(VulkanDevice* device, VkDeviceSize size, VkBufferUsageFlags usage,
                        VkMemoryPropertyFlags properties) {
    device_ = device;
    size_ = size;
    properties_ = properties;

    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaMemoryUsage memory_usage = VMA_MEMORY_USAGE_UNKNOWN;
    VkMemoryPropertyFlags required_flags = 0;
    if ((usage & VK_BUFFER_USAGE_TRANSFER_SRC_BIT) == VK_BUFFER_USAGE_TRANSFER_SRC_BIT &&
        (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        memory_usage = VMA_MEMORY_USAGE_CPU_ONLY;
    } else if ((properties & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
               (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        memory_usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
    } else if ((properties & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
        memory_usage = VMA_MEMORY_USAGE_GPU_ONLY;
    } else {
        required_flags = properties;
    }

    VmaAllocationCreateInfo create_info{};
    create_info.usage = memory_usage;
    create_info.requiredFlags = required_flags;

    if (vmaCreateBuffer(device_->allocator(), &info, &create_info, &buffer_, &allocation_, nullptr) != VK_SUCCESS) {
        GLOG_ERROR("VulkanBuffer: failed to create buffer");
        return false;
    }

    if (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        vmaMapMemory(device_->allocator(), allocation_, &mapped_);
    }

    VmaAllocationInfo alloc_info{};
    vmaGetAllocationInfo(device_->allocator(), allocation_, &alloc_info);
    VkPhysicalDeviceMemoryProperties mem_props{};
    vkGetPhysicalDeviceMemoryProperties(device_->physical_device(), &mem_props);
    is_coherent_ = (mem_props.memoryTypes[alloc_info.memoryType].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;

    return true;
}

void VulkanBuffer::shutdown() {
    if (!device_ || !device_->is_valid()) return;
    if (mapped_) {
        vmaUnmapMemory(device_->allocator(), allocation_);
        mapped_ = nullptr;
    }
    if (buffer_) {
        vmaDestroyBuffer(device_->allocator(), buffer_, allocation_);
        buffer_ = VK_NULL_HANDLE;
        allocation_ = nullptr;
    }
    device_ = nullptr;
}

bool VulkanBuffer::upload(const void* data, VkDeviceSize size, VkDeviceSize offset) const {
    if (!data || size == 0 || !buffer_) return false;
    if (mapped_) {
        std::memcpy(static_cast<char*>(mapped_) + offset, data, size);
        if (!is_coherent_) {
            vmaFlushAllocation(device_->allocator(), allocation_, offset, size);
        }
    } else {
        // device-local fallback：创建 staging buffer -> cmd copy -> device-local buffer
        VulkanBuffer staging;
        if (!staging.init(device_, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            GLOG_ERROR("VulkanBuffer::upload: failed to create staging buffer");
            return false;
        }
        staging.upload(data, size);

        VkDevice dev = device_->device();
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.queueFamilyIndex = device_->graphics_queue_family();
        pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        if (vkCreateCommandPool(dev, &pool_info, nullptr, &pool) != VK_SUCCESS) {
            GLOG_ERROR("VulkanBuffer::upload: failed to create command pool");
            return false;
        }

        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc_info.commandPool = pool;
        alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandBufferCount = 1;
        vkAllocateCommandBuffers(dev, &alloc_info, &cmd);

        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);

        VkBufferCopy region{};
        region.srcOffset = 0;
        region.dstOffset = offset;
        region.size = size;
        vkCmdCopyBuffer(cmd, staging.buffer_, buffer_, 1, &region);

        vkEndCommandBuffer(cmd);

        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence = VK_NULL_HANDLE;
        vkCreateFence(dev, &fence_info, nullptr, &fence);

        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        vkQueueSubmit(device_->graphics_queue(), 1, &submit, fence);
        vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);

        vkDestroyFence(dev, fence, nullptr);
        vkFreeCommandBuffers(dev, pool, 1, &cmd);
        vkDestroyCommandPool(dev, pool, nullptr);
    }
    return true;
}

} // namespace gryce_engine::render
