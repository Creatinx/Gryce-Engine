#include "vk_imgui_backend.h"

#include "render/render.h"
#include "vk_backend.h"
#include "vk_device.h"
#include "vk_swapchain.h"
#include "utils/glog/glog_lib.h"

#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>

// 引擎使用负 viewport height 匹配 OpenGL Y 向下；ImGui 自身使用正 viewport 与默认投影，
// 因此不需要在 imgui_impl_vulkan.cpp 中额外翻转。
bool g_imgui_vulkan_flip_viewport_y = false;

namespace gryce_engine::render {

VulkanImGuiBackend::VulkanImGuiBackend(IRenderBackend* backend) {
    backend_ = dynamic_cast<VulkanBackend*>(backend);
}

VulkanImGuiBackend::~VulkanImGuiBackend() {
    shutdown();
}

bool VulkanImGuiBackend::init() {
    if (!backend_) return false;

    VkDevice dev = backend_->device()->device();
    VkPhysicalDevice physical = backend_->device()->physical_device();

    ImGui_ImplVulkan_InitInfo init_info{};
    init_info.ApiVersion = VK_API_VERSION_1_2;
    init_info.Instance = backend_->instance();
    init_info.PhysicalDevice = physical;
    init_info.Device = dev;
    init_info.QueueFamily = backend_->device()->graphics_queue_family();
    init_info.Queue = backend_->device()->graphics_queue();
    init_info.DescriptorPoolSize = 64; // 让后端自动创建 descriptor pool
    init_info.MinImageCount = backend_->swapchain()->image_count();
    init_info.ImageCount = backend_->swapchain()->image_count();
    init_info.PipelineInfoMain.RenderPass = backend_->swapchain()->render_pass();
    init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

    if (!ImGui_ImplVulkan_Init(&init_info)) {
        GLOG_ERROR("VulkanImGuiBackend: ImGui_ImplVulkan_Init failed");
        return false;
    }

    initialized_ = true;
    GLOG_INFO("VulkanImGuiBackend initialized");
    return true;
}

void VulkanImGuiBackend::shutdown() {
    if (!initialized_ || !backend_) return;
    VkDevice dev = backend_->device()->device();
    if (dev != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(dev);
    }
    ImGui_ImplVulkan_Shutdown();
    initialized_ = false;
}

void VulkanImGuiBackend::new_frame() {
    ImGui_ImplVulkan_NewFrame();
}

void VulkanImGuiBackend::render_draw_data(ImDrawData* draw_data) {
    if (!draw_data || !backend_) return;
    VkCommandBuffer cmd = backend_->current_command_buffer();
    if (cmd == VK_NULL_HANDLE) return;
    ImGui_ImplVulkan_RenderDrawData(draw_data, cmd);
}

} // namespace gryce_engine::render
