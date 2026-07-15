#include "vk_device.h"

#include <cstring>
#include <vector>

#include <vma/vk_mem_alloc.h>

#include "utils/glog/glog_lib.h"

namespace gryce_engine::render {

namespace {

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

    VmaAllocatorCreateInfo allocator_info{};
    allocator_info.vulkanApiVersion = VK_API_VERSION_1_2;
    allocator_info.physicalDevice = physical_device_;
    allocator_info.device = device_;
    allocator_info.instance = instance;
    if (vmaCreateAllocator(&allocator_info, &allocator_) != VK_SUCCESS) {
        GLOG_ERROR("VulkanDevice: failed to create VMA allocator");
        return false;
    }

    GLOG_INFO("VulkanDevice created");
    return true;
}

void VulkanDevice::shutdown() {
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
            GLOG_INFO("VulkanDevice selected GPU: {} (extended_dynamic_state={}, anisotropy={})",
                      props.deviceName, supports_extended_dynamic_state_, supports_anisotropy_);
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
