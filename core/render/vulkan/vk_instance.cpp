#include "vk_instance.h"

#include <cstring>
#include <vector>

#include <GLFW/glfw3.h>

#include "utils/glog/glog_lib.h"

namespace gryce_engine::render {

bool VulkanInstance::enable_validation_ = false;

namespace {

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*type*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* /*user*/) {
    const char* msg = data ? data->pMessage : "unknown";
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        GLOG_ERROR("[Vulkan] {}", msg);
    } else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        GLOG_WARN("[Vulkan] {}", msg);
    } else {
        GLOG_INFO("[Vulkan] {}", msg);
    }
    return VK_FALSE;
}

VkResult create_debug_utils_messenger(
    VkInstance instance,
    const VkDebugUtilsMessengerCreateInfoEXT* info,
    const VkAllocationCallbacks* allocator,
    VkDebugUtilsMessengerEXT* messenger) {
    auto func = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
    return func ? func(instance, info, allocator, messenger) : VK_ERROR_EXTENSION_NOT_PRESENT;
}

void destroy_debug_utils_messenger(
    VkInstance instance,
    VkDebugUtilsMessengerEXT messenger,
    const VkAllocationCallbacks* allocator) {
    auto func = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
    if (func) {
        func(instance, messenger, allocator);
    }
}

} // namespace

bool VulkanInstance::init(const std::vector<const char*>& extensions) {
    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "Gryce Engine";
    app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.pEngineName = "Gryce Engine";
    app_info.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.apiVersion = VK_API_VERSION_1_2;

    std::vector<const char*> ext = extensions;

    // 校验层默认关闭，仅在显式开启且驱动支持时才启用
    const char* validation_layer = "VK_LAYER_KHRONOS_validation";
    validation_enabled_ = false;
    if (enable_validation_) {
        uint32_t layer_count = 0;
        vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
        std::vector<VkLayerProperties> layers(layer_count);
        vkEnumerateInstanceLayerProperties(&layer_count, layers.data());
        for (const auto& layer : layers) {
            if (std::strcmp(layer.layerName, validation_layer) == 0) {
                validation_enabled_ = true;
                break;
            }
        }
        if (!validation_enabled_) {
            GLOG_WARN("VulkanInstance: validation requested but layer not available");
        }
    }

    // 如果启用调试信使，必须声明 VK_EXT_debug_utils 扩展
    if (validation_enabled_) {
        bool has_debug_utils = false;
        for (const char* e : ext) {
            if (std::strcmp(e, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0) {
                has_debug_utils = true;
                break;
            }
        }
        if (!has_debug_utils) {
            ext.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }
    }

    VkInstanceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app_info;
    create_info.enabledExtensionCount = static_cast<uint32_t>(ext.size());
    create_info.ppEnabledExtensionNames = ext.data();

    VkDebugUtilsMessengerCreateInfoEXT debug_info{};
    debug_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    debug_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debug_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debug_info.pfnUserCallback = debug_callback;

    if (validation_enabled_) {
        create_info.enabledLayerCount = 1;
        create_info.ppEnabledLayerNames = &validation_layer;
        create_info.pNext = &debug_info;
    }

    if (vkCreateInstance(&create_info, nullptr, &instance_) != VK_SUCCESS) {
        GLOG_ERROR("VulkanInstance: failed to create Vulkan instance");
        return false;
    }

    if (validation_enabled_) {
        create_debug_messenger();
    }

    GLOG_INFO("VulkanInstance created with {} extensions", ext.size());
    for (const char* e : ext) {
        GLOG_INFO("  extension: {}", e);
    }
    if (validation_enabled_) {
        GLOG_INFO("VulkanInstance validation layer enabled");
    }
    return true;
}

void VulkanInstance::shutdown() {
    destroy_debug_messenger();
    if (instance_) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
}

bool VulkanInstance::create_debug_messenger() {
    VkDebugUtilsMessengerCreateInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = debug_callback;

    return create_debug_utils_messenger(instance_, &info, nullptr, &debug_messenger_) == VK_SUCCESS;
}

void VulkanInstance::destroy_debug_messenger() {
    if (debug_messenger_ != VK_NULL_HANDLE) {
        destroy_debug_utils_messenger(instance_, debug_messenger_, nullptr);
        debug_messenger_ = VK_NULL_HANDLE;
    }
}

} // namespace gryce_engine::render
