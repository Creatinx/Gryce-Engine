#pragma once

// ---------------------------------------------------------------------------
// vk_acceleration_structure — Vulkan 硬件光追加速结构构建（P2.1）
//
// 仅依赖 VK_KHR_acceleration_structure + VK_KHR_ray_query（设备支持时由
// VulkanDevice 启用）。本文件只在 GRYCE_HAS_VULKAN 下被编入，且调用点都做了
// 能力门控：不支持光追的设备不会实例化这里的东西，渲染路径自动回退 SSR。
//
// 设计取舍：构建输入直接吃"紧凑 xyz 位置 + uint32 索引"的裸数组，与 IMesh
// 解耦——P2.1 先不要求 IMesh 暴露 CPU 数据（那是 P2.2 的接线工作）。
// BLAS/TLAS 都走一次性命令提交 + fence 等待（典型在资源加载期调用，不在帧内）。
// ---------------------------------------------------------------------------

#include "render/vulkan/vk_buffer.h"

#include <vulkan/vulkan.h>
#include <vector>
#include <cstdint>

namespace gryce_engine::render {

class VulkanDevice;

// 加速结构构建输入：仅需要顶点位置（xyz 紧凑三元组）与三角形索引。
// 法线/UV 等其它属性 BLAS 不需要，光追命中阶段由着色器另行取用。
struct AccelerationStructureGeometry {
    const float* positions = nullptr;   // xyz * vertex_count
    uint32_t vertex_count = 0;
    const uint32_t* indices = nullptr;  // 3 * triangle_count
    uint32_t index_count = 0;
};

// 底级加速结构（BLAS）：一份几何的包围体层级（BVH）。
class VkBottomLevelAccelerationStructure {
public:
    VkBottomLevelAccelerationStructure() = default;
    ~VkBottomLevelAccelerationStructure();

    VkBottomLevelAccelerationStructure(const VkBottomLevelAccelerationStructure&) = delete;
    VkBottomLevelAccelerationStructure& operator=(const VkBottomLevelAccelerationStructure&) = delete;
    VkBottomLevelAccelerationStructure(VkBottomLevelAccelerationStructure&& other) noexcept;
    VkBottomLevelAccelerationStructure& operator=(VkBottomLevelAccelerationStructure&& other) noexcept;

    // 用紧凑 xyz 位置 + uint32 索引构建 BLAS。成功返回 true。
    bool build(VulkanDevice* device, const AccelerationStructureGeometry& geo);
    void destroy();

    VkAccelerationStructureKHR handle() const { return as_; }
    VkDeviceAddress device_address() const { return device_address_; }
    bool is_valid() const { return as_ != VK_NULL_HANDLE; }

private:
    VulkanDevice* device_ = nullptr;
    VkAccelerationStructureKHR as_ = VK_NULL_HANDLE;
    VkDeviceAddress device_address_ = 0;

    // AS 自身存储 + 构建临时 scratch + 几何缓冲（构建后保留供 AS 引用）。
    VulkanBuffer as_buffer_;
    VulkanBuffer scratch_buffer_;
    VulkanBuffer vertex_buffer_;
    VulkanBuffer index_buffer_;
};

// 顶级加速结构（TLAS）：持有实例列表，引用若干 BLAS。
class VkTopLevelAccelerationStructure {
public:
    VkTopLevelAccelerationStructure() = default;
    ~VkTopLevelAccelerationStructure();

    VkTopLevelAccelerationStructure(const VkTopLevelAccelerationStructure&) = delete;
    VkTopLevelAccelerationStructure& operator=(const VkTopLevelAccelerationStructure&) = delete;
    VkTopLevelAccelerationStructure(VkTopLevelAccelerationStructure&& other) noexcept;
    VkTopLevelAccelerationStructure& operator=(VkTopLevelAccelerationStructure&& other) noexcept;

    // 用实例列表构建/重建 TLAS。instances 中每个元素的
    // accelerationStructureReference 必须是 BLAS 的 device_address()（不是
    // VkAccelerationStructureKHR 句柄），transform 为行主序 3×4 矩阵。
    bool build(VulkanDevice* device,
               const std::vector<VkAccelerationStructureInstanceKHR>& instances);
    void destroy();

    VkAccelerationStructureKHR handle() const { return as_; }
    VkDeviceAddress device_address() const { return device_address_; }
    bool is_valid() const { return as_ != VK_NULL_HANDLE; }

private:
    bool build_internal(VulkanDevice* device, uint32_t instance_count,
                       const VkAccelerationStructureInstanceKHR* instances);

    VulkanDevice* device_ = nullptr;
    VkAccelerationStructureKHR as_ = VK_NULL_HANDLE;
    VkDeviceAddress device_address_ = 0;

    VulkanBuffer as_buffer_;
    VulkanBuffer scratch_buffer_;
    VulkanBuffer instance_buffer_;   // 实例数据（BUILD_INPUT_READ_ONLY）
};

} // namespace gryce_engine::render
