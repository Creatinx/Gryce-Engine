// ---------------------------------------------------------------------------
// vk_acceleration_structure.cpp — Vulkan 硬件光追加速结构构建（P2.1）
//
// 见 vk_acceleration_structure.h 的设计说明。构建流程遵循 VK_KHR_acceleration_
// structure 标准范式：查尺寸 → 建存储/scratch 缓冲 → 建 AS 对象 → 一次性命令
// 提交构建 → fence 等待。全部走 VMA（VulkanBuffer），销毁自动回收。
//
// 注意：VK_KHR_acceleration_structure 的设备扩展函数不在 vulkan-1.lib 中静态
// 导出，必须经 vkGetDeviceProcAddr 动态解析后才能调用（与引擎对
// VK_EXT_extended_dynamic_state 的 vkCmdSetXxxEXT 处理方式一致）。
// ---------------------------------------------------------------------------

#include "render/vulkan/vk_acceleration_structure.h"

#include "render/vulkan/vk_device.h"
#include "utils/glog/glog_lib.h"

#include <vma/vk_mem_alloc.h>

#include <cstring>

namespace gryce_engine::render {

namespace {

// 加速结构所需 KHR 设备扩展函数集合（vulkan-1.lib 不导出，须动态解析）。
struct VkASDispatch {
    PFN_vkCreateAccelerationStructureKHR vkCreateAccelerationStructureKHR = nullptr;
    PFN_vkDestroyAccelerationStructureKHR vkDestroyAccelerationStructureKHR = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR vkGetAccelerationStructureBuildSizesKHR = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR vkCmdBuildAccelerationStructuresKHR = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR vkGetAccelerationStructureDeviceAddressKHR = nullptr;

    bool resolve(VkDevice dev) {
        vkCreateAccelerationStructureKHR =
            reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(
                vkGetDeviceProcAddr(dev, "vkCreateAccelerationStructureKHR"));
        vkDestroyAccelerationStructureKHR =
            reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(
                vkGetDeviceProcAddr(dev, "vkDestroyAccelerationStructureKHR"));
        vkGetAccelerationStructureBuildSizesKHR =
            reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(
                vkGetDeviceProcAddr(dev, "vkGetAccelerationStructureBuildSizesKHR"));
        vkCmdBuildAccelerationStructuresKHR =
            reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(
                vkGetDeviceProcAddr(dev, "vkCmdBuildAccelerationStructuresKHR"));
        vkGetAccelerationStructureDeviceAddressKHR =
            reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(
                vkGetDeviceProcAddr(dev, "vkGetAccelerationStructureDeviceAddressKHR"));
        return vkCreateAccelerationStructureKHR && vkDestroyAccelerationStructureKHR &&
               vkGetAccelerationStructureBuildSizesKHR && vkCmdBuildAccelerationStructuresKHR &&
               vkGetAccelerationStructureDeviceAddressKHR;
    }
};

// 取缓冲的设备地址（光追构建与实例引用都需要，VK 1.2 核心函数，库内已导出）。
VkDeviceAddress buffer_device_address(VkDevice device, VkBuffer buffer) {
    if (buffer == VK_NULL_HANDLE) return 0;
    VkBufferDeviceAddressInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    info.buffer = buffer;
    return vkGetBufferDeviceAddress(device, &info);
}

// 一次性录制加速结构构建命令并提交 + 等待完成。
bool one_shot_build(VulkanDevice* device, const VkASDispatch& d,
                    VkAccelerationStructureBuildGeometryInfoKHR* build_infos,
                    const VkAccelerationStructureBuildRangeInfoKHR* const* ranges,
                    uint32_t info_count) {
    VkDevice dev = device->device();
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.queueFamilyIndex = device->graphics_queue_family();
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    if (vkCreateCommandPool(dev, &pool_info, nullptr, &pool) != VK_SUCCESS) {
        GLOG_ERROR("VkAccelerationStructure: failed to create command pool");
        return false;
    }

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    vkAllocateCommandBuffers(dev, &alloc, &cmd);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &begin) != VK_SUCCESS) {
        vkDestroyCommandPool(dev, pool, nullptr);
        GLOG_ERROR("VkAccelerationStructure: failed to begin command buffer");
        return false;
    }
    d.vkCmdBuildAccelerationStructuresKHR(cmd, info_count, build_infos, ranges);
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        vkDestroyCommandPool(dev, pool, nullptr);
        GLOG_ERROR("VkAccelerationStructure: failed to end command buffer");
        return false;
    }

    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkCreateFence(dev, &fence_info, nullptr, &fence);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    bool ok = vkQueueSubmit(device->graphics_queue(), 1, &submit, fence) == VK_SUCCESS &&
              vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;

    if (!ok) GLOG_ERROR("VkAccelerationStructure: build submit/wait failed");

    vkDestroyFence(dev, fence, nullptr);
    vkFreeCommandBuffers(dev, pool, 1, &cmd);
    vkDestroyCommandPool(dev, pool, nullptr);
    return ok;
}

VkDeviceAddress acceleration_structure_device_address(const VkASDispatch& d, VkDevice device,
                                                    VkAccelerationStructureKHR as) {
    if (as == VK_NULL_HANDLE) return 0;
    VkAccelerationStructureDeviceAddressInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    info.accelerationStructure = as;
    return d.vkGetAccelerationStructureDeviceAddressKHR(device, &info);
}

// 销毁时单独解析 destroy（避免依赖构建期 dispatch 的存活状态）。
void destroy_acceleration_structure(VkDevice dev, VkAccelerationStructureKHR as) {
    if (as == VK_NULL_HANDLE) return;
    auto fn = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(
        vkGetDeviceProcAddr(dev, "vkDestroyAccelerationStructureKHR"));
    if (fn) fn(dev, as, nullptr);
}

} // namespace

// ===========================================================================
// VkBottomLevelAccelerationStructure
// ===========================================================================

VkBottomLevelAccelerationStructure::~VkBottomLevelAccelerationStructure() {
    destroy();
}

VkBottomLevelAccelerationStructure::VkBottomLevelAccelerationStructure(
    VkBottomLevelAccelerationStructure&& other) noexcept
    : device_(other.device_)
    , as_(other.as_)
    , device_address_(other.device_address_)
    , as_buffer_(std::move(other.as_buffer_))
    , scratch_buffer_(std::move(other.scratch_buffer_))
    , vertex_buffer_(std::move(other.vertex_buffer_))
    , index_buffer_(std::move(other.index_buffer_)) {
    other.device_ = nullptr;
    other.as_ = VK_NULL_HANDLE;
    other.device_address_ = 0;
}

VkBottomLevelAccelerationStructure& VkBottomLevelAccelerationStructure::operator=(
    VkBottomLevelAccelerationStructure&& other) noexcept {
    if (this != &other) {
        destroy();
        device_ = other.device_;
        as_ = other.as_;
        device_address_ = other.device_address_;
        as_buffer_ = std::move(other.as_buffer_);
        scratch_buffer_ = std::move(other.scratch_buffer_);
        vertex_buffer_ = std::move(other.vertex_buffer_);
        index_buffer_ = std::move(other.index_buffer_);
        other.device_ = nullptr;
        other.as_ = VK_NULL_HANDLE;
        other.device_address_ = 0;
    }
    return *this;
}

bool VkBottomLevelAccelerationStructure::build(VulkanDevice* device,
                                              const AccelerationStructureGeometry& geo) {
    destroy();
    if (!device || !device->is_valid() || geo.vertex_count == 0 || geo.index_count == 0 ||
        !geo.positions || !geo.indices) {
        return false;
    }
    device_ = device;
    VkDevice dev = device->device();

    VkASDispatch d;
    if (!d.resolve(dev)) {
        GLOG_ERROR("VkAccelerationStructure: failed to resolve KHR functions (driver lacks RT?)");
        return false;
    }

    // 1) 位置 + 索引上传到 device-local 缓冲（VulkanBuffer::upload 自带 staging 回退）。
    const VkDeviceSize pos_size = static_cast<VkDeviceSize>(geo.vertex_count) * 3 * sizeof(float);
    const VkDeviceSize idx_size = static_cast<VkDeviceSize>(geo.index_count) * sizeof(uint32_t);
    if (!vertex_buffer_.init(device, pos_size,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
        return false;
    }
    if (!index_buffer_.init(device, idx_size,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
        return false;
    }
    if (!vertex_buffer_.upload(geo.positions, pos_size)) return false;
    if (!index_buffer_.upload(geo.indices, idx_size)) return false;

    // 2) 几何描述（仅位置 + 索引，opaque 不透明几何）。
    VkAccelerationStructureGeometryKHR geo_khr{};
    geo_khr.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geo_khr.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geo_khr.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    geo_khr.geometry.triangles.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    geo_khr.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    geo_khr.geometry.triangles.vertexData.deviceAddress =
        buffer_device_address(dev, vertex_buffer_.buffer());
    geo_khr.geometry.triangles.vertexStride = 3 * sizeof(float);
    geo_khr.geometry.triangles.maxVertex = geo.vertex_count - 1;
    geo_khr.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
    geo_khr.geometry.triangles.indexData.deviceAddress =
        buffer_device_address(dev, index_buffer_.buffer());

    VkAccelerationStructureBuildGeometryInfoKHR build_info{};
    build_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    build_info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    build_info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    build_info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    build_info.geometryCount = 1;
    build_info.pGeometries = &geo_khr;

    const uint32_t tri_count = geo.index_count / 3;
    VkAccelerationStructureBuildSizesInfoKHR build_sizes{};
    build_sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    d.vkGetAccelerationStructureBuildSizesKHR(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                            &build_info, &tri_count, &build_sizes);

    // 3) AS 存储缓冲 + scratch 缓冲。
    if (!as_buffer_.init(device, build_sizes.accelerationStructureSize,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
        return false;
    }
    if (!scratch_buffer_.init(device, build_sizes.buildScratchSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
        return false;
    }

    VkAccelerationStructureCreateInfoKHR create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    create_info.buffer = as_buffer_.buffer();
    create_info.size = build_sizes.accelerationStructureSize;
    create_info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    if (d.vkCreateAccelerationStructureKHR(dev, &create_info, nullptr, &as_) != VK_SUCCESS) {
        GLOG_ERROR("VkAccelerationStructure: failed to create BLAS");
        return false;
    }
    device_address_ = acceleration_structure_device_address(d, dev, as_);

    // 4) 一次性录制构建命令（dst + scratch 地址在命令级信息里给）。
    VkAccelerationStructureBuildGeometryInfoKHR build_cmd_info = build_info;
    build_cmd_info.dstAccelerationStructure = as_;
    build_cmd_info.scratchData.deviceAddress =
        buffer_device_address(dev, scratch_buffer_.buffer());

    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = tri_count;
    range.primitiveOffset = 0;
    range.firstVertex = 0;
    range.transformOffset = 0;
    const VkAccelerationStructureBuildRangeInfoKHR* ranges[] = { &range };

    if (!one_shot_build(device, d, &build_cmd_info, ranges, 1)) {
        destroy();
        return false;
    }
    return true;
}

void VkBottomLevelAccelerationStructure::destroy() {
    if (!device_ || !device_->is_valid()) {
        device_ = nullptr;
        return;
    }
    VkDevice dev = device_->device();
    if (as_ != VK_NULL_HANDLE) {
        destroy_acceleration_structure(dev, as_);
        as_ = VK_NULL_HANDLE;
    }
    device_address_ = 0;
    as_buffer_.shutdown();
    scratch_buffer_.shutdown();
    vertex_buffer_.shutdown();
    index_buffer_.shutdown();
    device_ = nullptr;
}

// ===========================================================================
// VkTopLevelAccelerationStructure
// ===========================================================================

VkTopLevelAccelerationStructure::~VkTopLevelAccelerationStructure() {
    destroy();
}

VkTopLevelAccelerationStructure::VkTopLevelAccelerationStructure(
    VkTopLevelAccelerationStructure&& other) noexcept
    : device_(other.device_)
    , as_(other.as_)
    , device_address_(other.device_address_)
    , as_buffer_(std::move(other.as_buffer_))
    , scratch_buffer_(std::move(other.scratch_buffer_))
    , instance_buffer_(std::move(other.instance_buffer_)) {
    other.device_ = nullptr;
    other.as_ = VK_NULL_HANDLE;
    other.device_address_ = 0;
}

VkTopLevelAccelerationStructure& VkTopLevelAccelerationStructure::operator=(
    VkTopLevelAccelerationStructure&& other) noexcept {
    if (this != &other) {
        destroy();
        device_ = other.device_;
        as_ = other.as_;
        device_address_ = other.device_address_;
        as_buffer_ = std::move(other.as_buffer_);
        scratch_buffer_ = std::move(other.scratch_buffer_);
        instance_buffer_ = std::move(other.instance_buffer_);
        other.device_ = nullptr;
        other.as_ = VK_NULL_HANDLE;
        other.device_address_ = 0;
    }
    return *this;
}

bool VkTopLevelAccelerationStructure::build(
    VulkanDevice* device,
    const std::vector<VkAccelerationStructureInstanceKHR>& instances) {
    destroy();
    if (!device || !device->is_valid() || instances.empty()) return false;
    device_ = device;
    return build_internal(device, static_cast<uint32_t>(instances.size()), instances.data());
}

bool VkTopLevelAccelerationStructure::build_internal(VulkanDevice* device,
                                                    uint32_t instance_count,
                                                    const VkAccelerationStructureInstanceKHR* instances) {
    VkDevice dev = device->device();

    VkASDispatch d;
    if (!d.resolve(dev)) {
        GLOG_ERROR("VkAccelerationStructure: failed to resolve KHR functions (driver lacks RT?)");
        return false;
    }

    // 实例数据缓冲（BUILD_INPUT_READ_ONLY + SHADER_DEVICE_ADDRESS）。
    const VkDeviceSize inst_size =
        static_cast<VkDeviceSize>(instance_count) * sizeof(VkAccelerationStructureInstanceKHR);
    if (!instance_buffer_.init(device, inst_size,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
        return false;
    }
    if (!instance_buffer_.upload(instances, inst_size)) return false;

    VkAccelerationStructureGeometryKHR geo_khr{};
    geo_khr.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geo_khr.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geo_khr.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    geo_khr.geometry.instances.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    geo_khr.geometry.instances.arrayOfPointers = VK_FALSE;
    geo_khr.geometry.instances.data.deviceAddress =
        buffer_device_address(dev, instance_buffer_.buffer());

    VkAccelerationStructureBuildGeometryInfoKHR build_info{};
    build_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    build_info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    build_info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    build_info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    build_info.geometryCount = 1;
    build_info.pGeometries = &geo_khr;

    VkAccelerationStructureBuildSizesInfoKHR build_sizes{};
    build_sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    d.vkGetAccelerationStructureBuildSizesKHR(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                            &build_info, &instance_count, &build_sizes);

    if (!as_buffer_.init(device, build_sizes.accelerationStructureSize,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
        return false;
    }
    if (!scratch_buffer_.init(device, build_sizes.buildScratchSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
        return false;
    }

    VkAccelerationStructureCreateInfoKHR create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    create_info.buffer = as_buffer_.buffer();
    create_info.size = build_sizes.accelerationStructureSize;
    create_info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    if (d.vkCreateAccelerationStructureKHR(dev, &create_info, nullptr, &as_) != VK_SUCCESS) {
        GLOG_ERROR("VkAccelerationStructure: failed to create TLAS");
        return false;
    }
    device_address_ = acceleration_structure_device_address(d, dev, as_);

    VkAccelerationStructureBuildGeometryInfoKHR build_cmd_info = build_info;
    build_cmd_info.dstAccelerationStructure = as_;
    build_cmd_info.scratchData.deviceAddress =
        buffer_device_address(dev, scratch_buffer_.buffer());

    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = instance_count;
    range.primitiveOffset = 0;
    range.firstVertex = 0;
    range.transformOffset = 0;
    const VkAccelerationStructureBuildRangeInfoKHR* ranges[] = { &range };

    if (!one_shot_build(device, d, &build_cmd_info, ranges, 1)) {
        destroy();
        return false;
    }
    return true;
}

void VkTopLevelAccelerationStructure::destroy() {
    if (!device_ || !device_->is_valid()) {
        device_ = nullptr;
        return;
    }
    VkDevice dev = device_->device();
    if (as_ != VK_NULL_HANDLE) {
        destroy_acceleration_structure(dev, as_);
        as_ = VK_NULL_HANDLE;
    }
    device_address_ = 0;
    as_buffer_.shutdown();
    scratch_buffer_.shutdown();
    instance_buffer_.shutdown();
    device_ = nullptr;
}

} // namespace gryce_engine::render
