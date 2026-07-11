#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <string>

namespace gryce_engine::render {

// ---------------------------------------------------------------------------
// VulkanInstance — Vulkan 实例 + 调试信使封装
// ---------------------------------------------------------------------------
class VulkanInstance {
public:
    // 默认关闭校验层以提升性能；可通过 --vulkan-validation 显式开启
    static void set_enable_validation(bool enable) { enable_validation_ = enable; }
    static bool enable_validation() { return enable_validation_; }

    bool init(const std::vector<const char*>& extensions);
    void shutdown();

    VkInstance handle() const { return instance_; }
    bool is_valid() const { return instance_ != VK_NULL_HANDLE; }
    bool validation_enabled() const { return validation_enabled_; }

private:
    bool create_debug_messenger();
    void destroy_debug_messenger();

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug_messenger_ = VK_NULL_HANDLE;
    bool validation_enabled_ = false;
    static bool enable_validation_;
};

} // namespace gryce_engine::render
