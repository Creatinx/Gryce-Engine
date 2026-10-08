#pragma once

#include "render/buffer.h"

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

// ---------------------------------------------------------------------------
// VulkanStorageBuffer — SSBO / 间接绘制参数缓冲
// ---------------------------------------------------------------------------
// GPU 剔除（compute 写 SSBO + 写 DrawArraysIndirectCommand）与间接绘制共用
// 的缓冲。三个用途位同时给：STORAGE（compute 读写）/ VERTEX（剔除结果作为
// 每实例属性流）/ INDIRECT（绘制参数）。
//
// 多槽位轮转：CPU 每帧都要覆写实例矩阵与绘制参数，而 GPU 可能还在执行上一
// 周期的命令缓冲（在途帧 > 1）。单缓冲会出现"CPU 写、GPU 同时读"的数据竞争，
// 表现为实例矩阵撕裂/闪烁。这里按在途帧数开 k_slots 个等大区域，每帧轮转：
//   - CPU 写：IBuffer::update(data, size, offset) 内部加上 frame_offset()；
//   - GPU 读：描述符（VkDescriptorBufferInfo.offset）与 vkCmdDrawIndirect 的
//     偏移同样加上 frame_offset()，着色器看到的仍是从 0 开始的一段。
// 轮转发生在 begin_frame（此时 frame fence 已等过），因此复用区域必定已空闲。
class VulkanStorageBuffer : public IBuffer {
public:
    explicit VulkanStorageBuffer(VulkanDevice* device) : device_(device) {}
    ~VulkanStorageBuffer() override { destroy(); }

    VulkanStorageBuffer(const VulkanStorageBuffer&) = delete;
    VulkanStorageBuffer& operator=(const VulkanStorageBuffer&) = delete;

    bool create(size_t size, const void* data, BufferUsage usage) override;
    void update(const void* data, size_t size, size_t offset = 0) override;
    // Vulkan 没有 GL 那样的 "bind to binding point"：SSBO 通过描述符集绑定。
    void bind(uint32_t /*binding_point*/) const override {}
    void destroy() override;

    size_t size() const override { return size_; }
    bool is_valid() const override { return buffer_.buffer() != VK_NULL_HANDLE; }

    VkBuffer vk_buffer() const { return buffer_.buffer(); }
    // 当前帧区域的字节偏移（CPU 写入与 GPU 读取都用它）
    size_t frame_offset() const { return static_cast<size_t>(slot_) * size_; }
    void on_begin_frame() { slot_ = (slot_ + 1) % k_slots; }

private:
    // 与 VulkanSwapchain 的在途帧数一致（3）
    static constexpr uint32_t k_slots = 3;

    VulkanDevice* device_ = nullptr;
    VulkanBuffer buffer_;
    size_t size_ = 0;
    uint32_t slot_ = 0;
};

} // namespace gryce_engine::render
