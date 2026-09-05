#include "vk_imgui_backend.h"

#include "render/render.h"
#include "render/texture.h"
#include "vk_backend.h"
#include "vk_device.h"
#include "vk_swapchain.h"
#include "vk_texture.h"
#include "utils/glog/glog_lib.h"

#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>

// 引擎使用负 viewport height 匹配 OpenGL Y 向下；ImGui 自身使用正 viewport 与默认投影，
// 因此不需要在 imgui_impl_vulkan.cpp 中额外翻转。
bool g_imgui_vulkan_flip_viewport_y = false;

namespace gryce_engine::render {

VulkanImGuiBackend::VulkanImGuiBackend(IRenderBackend* backend) {
    backend_ = dynamic_cast<VulkanBackend*>(backend);
    if (backend_) {
        backend_->set_imgui_backend(this);
    }
}

VulkanImGuiBackend::~VulkanImGuiBackend() {
    shutdown();
    if (backend_) {
        backend_->set_imgui_backend(nullptr);
    }
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
    clear_cache();
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

void VulkanImGuiBackend::rebuild_fonts() {
    if (!initialized_ || !backend_) return;
    VkDevice dev = backend_->device()->device();
    if (dev != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(dev);
    }
    // 新版 ImGui 使用动态纹理机制：手动触发字体 atlas 纹理上传。
    ImTextureData* font_tex = ImGui::GetIO().Fonts->TexRef._TexData;
    if (font_tex) {
        ImGui_ImplVulkan_UpdateTexture(font_tex);
    }
}

uint64_t VulkanImGuiBackend::imgui_texture_id(ITexture* texture) const {
    if (!texture || !texture->is_valid() || !initialized_) return 0;

    auto* vk_tex = dynamic_cast<VulkanTexture*>(texture);
    if (!vk_tex) return 0;

    // 缓存命中且纹理仍有效：移到 LRU 前端并返回。
    auto it = cache_map_.find(texture);
    if (it != cache_map_.end()) {
        if (it->second->texture->is_valid()) {
            cache_lru_.splice(cache_lru_.begin(), cache_lru_, it->second);
            return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(it->second->set));
        }
        // 纹理已失效：移除旧缓存项，后续重新创建。
        cache_lru_.erase(it->second);
        cache_map_.erase(it);
    }

    VkDescriptorSet set = ImGui_ImplVulkan_AddTexture(
        vk_tex->sampler(), vk_tex->image_view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (set == VK_NULL_HANDLE) {
        GLOG_ERROR("VulkanImGuiBackend: failed to create descriptor set for texture");
        return 0;
    }

    CacheEntry entry;
    entry.texture = texture;
    entry.set = set;
    cache_lru_.push_front(entry);
    cache_map_[texture] = cache_lru_.begin();

    // 限制缓存大小，避免 descriptor set 无限增长。
    while (cache_lru_.size() > k_max_cache_size) {
        const CacheEntry& oldest = cache_lru_.back();
        if (oldest.set != VK_NULL_HANDLE) {
            ImGui_ImplVulkan_RemoveTexture(oldest.set);
        }
        cache_map_.erase(oldest.texture);
        cache_lru_.pop_back();
    }

    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(set));
}

void VulkanImGuiBackend::invalidate_texture(ITexture* tex) {
    if (!tex) return;
    auto it = cache_map_.find(tex);
    if (it == cache_map_.end()) return;
    if (it->second->set != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(it->second->set);
    }
    cache_lru_.erase(it->second);
    cache_map_.erase(it);
}

void VulkanImGuiBackend::clear_cache() {
    for (const auto& entry : cache_lru_) {
        if (entry.set != VK_NULL_HANDLE) {
            ImGui_ImplVulkan_RemoveTexture(entry.set);
        }
    }
    cache_lru_.clear();
    cache_map_.clear();
}

} // namespace gryce_engine::render
