#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

struct VmaAllocation_T;
using VmaAllocation = VmaAllocation_T*;

namespace gryce_engine::render {

class VulkanDevice;

// ---------------------------------------------------------------------------
// VulkanSwapchain — 交换链 + image views + framebuffers（极简骨架）
// ---------------------------------------------------------------------------
class VulkanSwapchain {
public:
    bool init(VkInstance instance, VulkanDevice* device, VkSurfaceKHR surface,
              uint32_t width, uint32_t height);
    void shutdown();

    // 窗口大小变化时重建 swapchain
    bool recreate(uint32_t width, uint32_t height);

    // 设置 VSync 开关并重建 swapchain
    void set_vsync_enabled(bool enabled);
    bool vsync_enabled() const { return present_mode_ == VK_PRESENT_MODE_FIFO_KHR; }

    // 获取下一帧 image 索引
    // 获取下一帧 image 索引，使用内部按帧轮换的信号量/栅栏
    VkResult acquire_next_image(uint32_t* image_index);
    // 呈现
    VkResult present(uint32_t image_index, VkSemaphore wait_semaphore);

    // 帧提交：由 backend 在 end_frame 中调用
    // 返回 present 结果，调用方据此处理 OUT_OF_DATE/SUBOPTIMAL
    VkResult submit_and_present(uint32_t image_index, VkCommandBuffer cmd);

    // 当前 frame 的栅栏（用于截图分支手动同步）
    VkFence current_frame_fence() const { return frame_fences_[current_frame_]; }
    // 手动推进帧索引（截图分支在手动 present 后调用）
    void advance_frame();

    VkRenderPass render_pass() const { return render_pass_; }
    VkFramebuffer framebuffer(uint32_t index) const { return framebuffers_[index]; }
    VkExtent2D extent() const { return extent_; }
    VkFormat format() const { return format_; }
    uint32_t image_count() const { return static_cast<uint32_t>(images_.size()); }
    VkImage image(uint32_t index) const { return images_[index]; }
    VkCommandBuffer command_buffer(uint32_t index) const { return command_buffers_[index]; }
    VkCommandPool command_pool() const { return command_pool_; }
    VkCommandBuffer secondary_command_buffer(uint32_t index) const { return secondary_command_buffers_[index]; }
    VkCommandPool secondary_command_pool() const { return secondary_pool_; }
    int current_frame_index() const { return current_frame_; }
    VkSemaphore current_image_available_semaphore() const { return image_available_semaphores_[current_frame_]; }
    VkSemaphore current_render_finished_semaphore() const { return render_finished_semaphores_[current_frame_]; }
    VkFence current_fence() const { return frame_fences_[current_frame_]; }
    int frames_in_flight() const { return frames_in_flight_; }

private:
    bool create_swapchain(uint32_t width, uint32_t height);
    bool create_image_views();
    bool create_depth_attachment();
    bool create_render_pass();
    bool create_framebuffers();
    bool create_command_buffer();
    bool create_secondary_command_buffer();
    bool create_sync_objects();

    VkInstance instance_ = VK_NULL_HANDLE;
    VulkanDevice* device_ = nullptr;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    std::vector<VkImage> images_;
    std::vector<VkImageView> image_views_;
    std::vector<VkFramebuffer> framebuffers_;
    VkFormat format_ = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D extent_{};
    VkPresentModeKHR present_mode_ = VK_PRESENT_MODE_IMMEDIATE_KHR;

    // 每个 swapchain image 独立 depth attachment，避免多帧并行时 depth buffer 冲突导致闪烁
    std::vector<VkImage> depth_images_;
    std::vector<VmaAllocation> depth_allocations_;
    std::vector<VkImageView> depth_views_;
    VkFormat depth_format_ = VK_FORMAT_D32_SFLOAT;

    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> command_buffers_;

    // secondary command buffers 池：供 inline accumulator 与 per-draw_mesh 使用
    VkCommandPool secondary_pool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> secondary_command_buffers_;

    static constexpr int kMaxFramesInFlight = 8;
    int frames_in_flight_ = 2;
    int current_frame_ = 0;
    uint32_t current_image_ = 0;
    std::vector<VkSemaphore> image_available_semaphores_;
    std::vector<VkSemaphore> render_finished_semaphores_;
    std::vector<VkFence> frame_fences_;
};

} // namespace gryce_engine::render
