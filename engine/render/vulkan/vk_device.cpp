#include "vk_device.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <vma/vk_mem_alloc.h>

#include "utils/glog/glog_lib.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace gryce_engine::render {

namespace {

// 磁盘管线缓存格式：自描述头 + 驱动产出的 VkPipelineCache 原始数据。
// 头里记录设备指纹，避免换显卡/换驱动后把不兼容的缓存喂给 vkCreatePipelineCache。
constexpr uint32_t k_pipeline_cache_magic = 0x47524350u; // 'GRPC'
constexpr uint32_t k_pipeline_cache_header_version = 1;

struct PipelineCacheFileHeader {
    uint32_t magic = k_pipeline_cache_magic;
    uint32_t header_version = k_pipeline_cache_header_version;
    uint32_t vendor_id = 0;
    uint32_t device_id = 0;
    uint32_t driver_version = 0;
    uint8_t cache_uuid[VK_UUID_SIZE] = {};
};

// 当前可执行文件所在目录；取不到则返回空（调用方退化为不落盘）。
std::filesystem::path exe_directory() {
#if defined(_WIN32)
    wchar_t buffer[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, buffer, MAX_PATH) > 0) {
        return std::filesystem::path(buffer).parent_path();
    }
#else
    std::error_code ec;
    const std::filesystem::path p = std::filesystem::canonical("/proc/self/exe", ec);
    if (!ec) return p.parent_path();
#endif
    return {};
}

// 管线缓存文件放在可执行文件同目录（每个发布目录 / 每个构建产物各自独立）。
std::filesystem::path pipeline_cache_file() {
    const std::filesystem::path dir = exe_directory();
    if (dir.empty()) return {};
    return dir / "vulkan_pipeline_cache.bin";
}

bool find_queue_families(VkPhysicalDevice device, VkSurfaceKHR surface,
                         uint32_t& graphics_family, uint32_t& present_family) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());

    graphics_family = UINT32_MAX;
    present_family = UINT32_MAX;
    for (uint32_t i = 0; i < count; ++i) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            graphics_family = i;
        }
        VkBool32 present_support = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &present_support);
        if (present_support) {
            present_family = i;
        }
        if (graphics_family != UINT32_MAX && present_family != UINT32_MAX) {
            return true;
        }
    }
    return false;
}

} // namespace

bool VulkanDevice::init(VkInstance instance, VkSurfaceKHR surface) {
    surface_ = surface;
    if (!pick_physical_device(instance, surface)) {
        return false;
    }
    if (!create_logical_device()) {
        return false;
    }
    vkGetDeviceQueue(device_, graphics_family_, 0, &graphics_queue_);
    vkGetDeviceQueue(device_, present_family_, 0, &present_queue_);

    VmaVulkanFunctions vulkan_functions{};
    // 动态导入模式(VMA_DYNAMIC_VULKAN_FUNCTIONS)必须显式提供这两入口，
    // 其余成员可 null，VMA 会通过它们自行获取。
    vulkan_functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    vulkan_functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    vulkan_functions.vkGetPhysicalDeviceProperties = vkGetPhysicalDeviceProperties;
    vulkan_functions.vkGetPhysicalDeviceMemoryProperties = vkGetPhysicalDeviceMemoryProperties;
    vulkan_functions.vkAllocateMemory = vkAllocateMemory;
    vulkan_functions.vkFreeMemory = vkFreeMemory;
    vulkan_functions.vkMapMemory = vkMapMemory;
    vulkan_functions.vkUnmapMemory = vkUnmapMemory;
    vulkan_functions.vkFlushMappedMemoryRanges = vkFlushMappedMemoryRanges;
    vulkan_functions.vkInvalidateMappedMemoryRanges = vkInvalidateMappedMemoryRanges;
    vulkan_functions.vkBindBufferMemory = vkBindBufferMemory;
    vulkan_functions.vkBindImageMemory = vkBindImageMemory;
    vulkan_functions.vkGetBufferMemoryRequirements = vkGetBufferMemoryRequirements;
    vulkan_functions.vkGetImageMemoryRequirements = vkGetImageMemoryRequirements;
    vulkan_functions.vkCreateBuffer = vkCreateBuffer;
    vulkan_functions.vkDestroyBuffer = vkDestroyBuffer;
    vulkan_functions.vkCreateImage = vkCreateImage;
    vulkan_functions.vkDestroyImage = vkDestroyImage;
    vulkan_functions.vkCmdCopyBuffer = vkCmdCopyBuffer;
    vulkan_functions.vkGetBufferMemoryRequirements2KHR = vkGetBufferMemoryRequirements2;
    vulkan_functions.vkGetImageMemoryRequirements2KHR = vkGetImageMemoryRequirements2;
    vulkan_functions.vkBindBufferMemory2KHR = vkBindBufferMemory2;
    vulkan_functions.vkBindImageMemory2KHR = vkBindImageMemory2;
    vulkan_functions.vkGetPhysicalDeviceMemoryProperties2KHR = vkGetPhysicalDeviceMemoryProperties2;

    VmaAllocatorCreateInfo allocator_info{};
    allocator_info.vulkanApiVersion = VK_API_VERSION_1_2;
    allocator_info.physicalDevice = physical_device_;
    allocator_info.device = device_;
    allocator_info.instance = instance;
    allocator_info.pVulkanFunctions = &vulkan_functions;
    if (vmaCreateAllocator(&allocator_info, &allocator_) != VK_SUCCESS) {
        GLOG_ERROR("VulkanDevice: failed to create VMA allocator");
        return false;
    }

    create_pipeline_cache();

    GLOG_INFO("VulkanDevice created");
    return true;
}

// 读取磁盘缓存并交给驱动；文件缺失/设备指纹不匹配则退化为空缓存。
void VulkanDevice::create_pipeline_cache() {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physical_device_, &props);

    std::vector<char> initial_data;
    const std::filesystem::path path = pipeline_cache_file();
    if (!path.empty()) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (file.is_open()) {
            const std::streamsize size = file.tellg();
            file.seekg(0, std::ios::beg);
            PipelineCacheFileHeader header{};
            const std::streamsize header_size = static_cast<std::streamsize>(sizeof(header));
            if (size >= header_size &&
                file.read(reinterpret_cast<char*>(&header), header_size)) {
                const bool compatible =
                    header.magic == k_pipeline_cache_magic &&
                    header.header_version == k_pipeline_cache_header_version &&
                    header.vendor_id == props.vendorID &&
                    header.device_id == props.deviceID &&
                    header.driver_version == props.driverVersion &&
                    std::memcmp(header.cache_uuid, props.pipelineCacheUUID, VK_UUID_SIZE) == 0;
                if (compatible) {
                    const std::streamsize data_size = size - header_size;
                    if (data_size > 0) {
                        initial_data.resize(static_cast<size_t>(data_size));
                        if (!file.read(initial_data.data(), data_size)) {
                            initial_data.clear();
                        }
                    }
                } else {
                    GLOG_INFO("VulkanDevice: pipeline cache file targets another device/driver, ignored");
                }
            }
        }
    }

    VkPipelineCacheCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    info.initialDataSize = initial_data.size();
    info.pInitialData = initial_data.empty() ? nullptr : initial_data.data();
    if (vkCreatePipelineCache(device_, &info, nullptr, &pipeline_cache_) != VK_SUCCESS) {
        pipeline_cache_ = VK_NULL_HANDLE;
        GLOG_WARN("VulkanDevice: failed to create pipeline cache; pipelines will be compiled every run");
        return;
    }

    if (initial_data.empty()) {
        GLOG_INFO("VulkanDevice: pipeline cache created (cold start)");
    } else {
        GLOG_INFO("VulkanDevice: pipeline cache loaded ({} bytes)", initial_data.size());
    }
}

// 关闭前把驱动编译产物写回磁盘，下次启动即命中，省掉全部 SPIR-V → ISA 编译。
void VulkanDevice::save_pipeline_cache() {
    if (!device_ || pipeline_cache_ == VK_NULL_HANDLE) return;

    size_t size = 0;
    if (vkGetPipelineCacheData(device_, pipeline_cache_, &size, nullptr) != VK_SUCCESS || size == 0) {
        return;
    }
    std::vector<char> blob(size);
    if (vkGetPipelineCacheData(device_, pipeline_cache_, &size, blob.data()) != VK_SUCCESS || size == 0) {
        return;
    }
    blob.resize(size);

    const std::filesystem::path path = pipeline_cache_file();
    if (path.empty()) return;

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physical_device_, &props);
    PipelineCacheFileHeader header{};
    header.vendor_id = props.vendorID;
    header.device_id = props.deviceID;
    header.driver_version = props.driverVersion;
    std::memcpy(header.cache_uuid, props.pipelineCacheUUID, VK_UUID_SIZE);

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        GLOG_WARN("VulkanDevice: failed to open pipeline cache for writing ('{}')", path.string());
        return;
    }
    file.write(reinterpret_cast<const char*>(&header), static_cast<std::streamsize>(sizeof(header)));
    file.write(blob.data(), static_cast<std::streamsize>(blob.size()));
    if (!file) {
        GLOG_WARN("VulkanDevice: failed to write pipeline cache ('{}')", path.string());
        return;
    }
    GLOG_INFO("VulkanDevice: pipeline cache saved ({} bytes)", blob.size());
}

void VulkanDevice::shutdown() {
    if (device_ && pipeline_cache_ != VK_NULL_HANDLE) {
        save_pipeline_cache();
        vkDestroyPipelineCache(device_, pipeline_cache_, nullptr);
        pipeline_cache_ = VK_NULL_HANDLE;
    }
    if (allocator_) {
        vmaDestroyAllocator(allocator_);
        allocator_ = nullptr;
    }
    if (device_) {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    physical_device_ = VK_NULL_HANDLE;
    graphics_queue_ = VK_NULL_HANDLE;
    present_queue_ = VK_NULL_HANDLE;
}

bool VulkanDevice::pick_physical_device(VkInstance instance, VkSurfaceKHR surface) {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    if (count == 0) {
        GLOG_ERROR("VulkanDevice: no physical devices found");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());

    for (const auto& device : devices) {
        uint32_t graphics = UINT32_MAX, present = UINT32_MAX;
        if (find_queue_families(device, surface, graphics, present)) {
            physical_device_ = device;
            graphics_family_ = graphics;
            present_family_ = present;

            // 检查 VK_EXT_extended_dynamic_state 支持
            uint32_t ext_count = 0;
            vkEnumerateDeviceExtensionProperties(device, nullptr, &ext_count, nullptr);
            std::vector<VkExtensionProperties> exts(ext_count);
            vkEnumerateDeviceExtensionProperties(device, nullptr, &ext_count, exts.data());
            for (const auto& ext : exts) {
                if (std::strcmp(ext.extensionName, VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME) == 0) {
                    supports_extended_dynamic_state_ = true;
                    break;
                }
            }

            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(device, &props);
            max_sampler_anisotropy_ = props.limits.maxSamplerAnisotropy;
            supports_anisotropy_ = max_sampler_anisotropy_ > 1.0f;
            max_push_constants_size_ = props.limits.maxPushConstantsSize;
            GLOG_INFO("VulkanDevice selected GPU: {} (extended_dynamic_state={}, anisotropy={}, max_push_constants={})",
                      props.deviceName, supports_extended_dynamic_state_, supports_anisotropy_,
                      max_push_constants_size_);
            return true;
        }
    }

    GLOG_ERROR("VulkanDevice: no suitable physical device");
    return false;
}

bool VulkanDevice::create_logical_device() {
    float priority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queue_infos;
    std::vector<uint32_t> unique_families = {graphics_family_};
    if (present_family_ != graphics_family_) {
        unique_families.push_back(present_family_);
    }

    for (uint32_t family : unique_families) {
        VkDeviceQueueCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        info.queueFamilyIndex = family;
        info.queueCount = 1;
        info.pQueuePriorities = &priority;
        queue_infos.push_back(info);
    }

    VkPhysicalDeviceFeatures features{};
    features.samplerAnisotropy = supports_anisotropy_ ? VK_TRUE : VK_FALSE;

    std::vector<const char*> device_extensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    // Query descriptor indexing support (Vulkan 1.2).
    VkPhysicalDeviceDescriptorIndexingFeatures indexing_features{};
    indexing_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &indexing_features;
    vkGetPhysicalDeviceFeatures2(physical_device_, &features2);

    bool supports_descriptor_indexing =
        indexing_features.descriptorBindingSampledImageUpdateAfterBind == VK_TRUE &&
        indexing_features.descriptorBindingUniformBufferUpdateAfterBind == VK_TRUE;
    if (supports_descriptor_indexing) {
        indexing_features.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
        indexing_features.descriptorBindingUniformBufferUpdateAfterBind = VK_TRUE;
        indexing_features.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    } else {
        GLOG_WARN("VulkanDevice: descriptor indexing update-after-bind not fully supported");
    }

    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT dynamic_state_features{};
    dynamic_state_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT;

    if (supports_extended_dynamic_state_) {
        device_extensions.push_back(VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME);
    }

    VkDeviceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create_info.queueCreateInfoCount = static_cast<uint32_t>(queue_infos.size());
    create_info.pQueueCreateInfos = queue_infos.data();
    create_info.pEnabledFeatures = &features;
    create_info.enabledExtensionCount = static_cast<uint32_t>(device_extensions.size());
    create_info.ppEnabledExtensionNames = device_extensions.data();

    if (supports_descriptor_indexing) {
        create_info.pNext = &indexing_features;
        indexing_features.pNext = supports_extended_dynamic_state_ ? &dynamic_state_features : nullptr;
    } else if (supports_extended_dynamic_state_) {
        create_info.pNext = &dynamic_state_features;
    }

    if (supports_extended_dynamic_state_) {
        dynamic_state_features.extendedDynamicState = VK_TRUE;
    }

    if (vkCreateDevice(physical_device_, &create_info, nullptr, &device_) != VK_SUCCESS) {
        GLOG_ERROR("VulkanDevice: failed to create logical device");
        return false;
    }
    return true;
}

} // namespace gryce_engine::render
