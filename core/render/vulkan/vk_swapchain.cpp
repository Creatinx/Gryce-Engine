#include "vk_swapchain.h"

#include <algorithm>
#include <limits>
#include <vector>

#include <vma/vk_mem_alloc.h>

#include "vk_device.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::render {

bool VulkanSwapchain::init(VkInstance instance, VulkanDevice* device,
                           VkSurfaceKHR surface, uint32_t width, uint32_t height) {
    instance_ = instance;
    device_ = device;
    surface_ = surface;

    if (!create_swapchain(width, height)) return false;
    if (!create_image_views()) return false;
    if (!create_depth_attachment()) return false;
    if (!create_render_pass()) return false;
    if (!create_framebuffers()) return false;
    if (!create_command_buffer()) return false;
    if (!create_secondary_command_buffer()) return false;
    if (!create_sync_objects()) return false;

    GLOG_INFO("VulkanSwapchain created: {}x{} with depth attachment", width, height);
    return true;
}

bool VulkanSwapchain::recreate(uint32_t width, uint32_t height) {
    if (!device_ || !device_->device()) return false;
    vkDeviceWaitIdle(device_->device());
    shutdown();
    return init(instance_, device_, surface_, width, height);
}

void VulkanSwapchain::set_vsync_enabled(bool enabled) {
    VkPresentModeKHR desired = enabled ? VK_PRESENT_MODE_FIFO_KHR : VK_PRESENT_MODE_IMMEDIATE_KHR;
    if (present_mode_ == desired) return;

    // 查询设备支持的呈现模式，若目标不可用则回退
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(device_->physical_device(), surface_, &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    if (count > 0) {
        vkGetPhysicalDeviceSurfacePresentModesKHR(device_->physical_device(), surface_, &count, modes.data());
    }

    bool supported = false;
    for (auto m : modes) {
        if (m == desired) {
            supported = true;
            break;
        }
    }

    if (!supported) {
        GLOG_WARN("VulkanSwapchain: requested present mode {} not supported, fallback to FIFO",
                  enabled ? "FIFO" : "IMMEDIATE");
        present_mode_ = VK_PRESENT_MODE_FIFO_KHR;
    } else {
        present_mode_ = desired;
    }

    recreate(extent_.width, extent_.height);
    GLOG_INFO("VulkanSwapchain: VSync {}", enabled ? "enabled" : "disabled");
}

void VulkanSwapchain::shutdown() {
    if (!device_ || !device_->device()) return;
    VkDevice dev = device_->device();

    for (auto fence : frame_fences_) {
        if (fence) vkDestroyFence(dev, fence, nullptr);
    }
    frame_fences_.clear();
    for (auto sem : render_finished_semaphores_) {
        if (sem) vkDestroySemaphore(dev, sem, nullptr);
    }
    render_finished_semaphores_.clear();
    for (auto sem : image_available_semaphores_) {
        if (sem) vkDestroySemaphore(dev, sem, nullptr);
    }
    image_available_semaphores_.clear();

    if (secondary_pool_) vkDestroyCommandPool(dev, secondary_pool_, nullptr);
    secondary_pool_ = VK_NULL_HANDLE;
    secondary_command_buffers_.clear();

    if (command_pool_) vkDestroyCommandPool(dev, command_pool_, nullptr);
    command_pool_ = VK_NULL_HANDLE;
    command_buffers_.clear();

    for (auto fb : framebuffers_) {
        if (fb) vkDestroyFramebuffer(dev, fb, nullptr);
    }
    framebuffers_.clear();

    if (render_pass_) vkDestroyRenderPass(dev, render_pass_, nullptr);
    render_pass_ = VK_NULL_HANDLE;
    if (render_pass_load_) vkDestroyRenderPass(dev, render_pass_load_, nullptr);
    render_pass_load_ = VK_NULL_HANDLE;

    for (auto view : depth_views_) {
        if (view) vkDestroyImageView(dev, view, nullptr);
    }
    depth_views_.clear();
    for (size_t i = 0; i < depth_images_.size(); ++i) {
        if (depth_images_[i]) {
            vmaDestroyImage(device_->allocator(), depth_images_[i], depth_allocations_[i]);
        }
    }
    depth_images_.clear();
    depth_allocations_.clear();

    for (auto view : image_views_) {
        if (view) vkDestroyImageView(dev, view, nullptr);
    }
    image_views_.clear();
    images_.clear();

    if (swapchain_) vkDestroySwapchainKHR(dev, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

bool VulkanSwapchain::create_swapchain(uint32_t width, uint32_t height) {
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device_->physical_device(), surface_, &caps);

    VkExtent2D extent{};
    if (caps.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
        extent = caps.currentExtent;
    } else {
        extent.width = std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    extent_ = extent;

    uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && image_count > caps.maxImageCount) {
        image_count = caps.maxImageCount;
    }

    VkSwapchainCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = surface_;
    info.minImageCount = image_count;
    info.imageFormat = format_;
    info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    info.imageExtent = extent_;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = present_mode_;
    info.clipped = VK_TRUE;
    info.oldSwapchain = VK_NULL_HANDLE;

    uint32_t families[] = {device_->graphics_queue_family(), device_->present_queue_family()};
    if (families[0] != families[1]) {
        info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = 2;
        info.pQueueFamilyIndices = families;
    } else {
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    uint32_t format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device_->physical_device(), surface_, &format_count, nullptr);
    std::vector<VkSurfaceFormatKHR> supported_formats(format_count);
    if (format_count > 0) {
        vkGetPhysicalDeviceSurfaceFormatsKHR(device_->physical_device(), surface_, &format_count, supported_formats.data());
    }
    {
        std::string fmt_list;
        for (size_t i = 0; i < supported_formats.size(); ++i) {
            if (i > 0) fmt_list += ", ";
            fmt_list += "(" + std::to_string(supported_formats[i].format) +
                        "," + std::to_string(supported_formats[i].colorSpace) + ")";
        }
        GLOG_INFO("VulkanSwapchain: supported surface formats=[{}]", fmt_list);
    }

    if (vkCreateSwapchainKHR(device_->device(), &info, nullptr, &swapchain_) != VK_SUCCESS) {
        GLOG_ERROR("VulkanSwapchain: failed to create swapchain");
        return false;
    }

    GLOG_INFO("VulkanSwapchain: selected format={} extent={}x{}",
              static_cast<int>(format_), extent.width, extent.height);

    uint32_t count = 0;
    vkGetSwapchainImagesKHR(device_->device(), swapchain_, &count, nullptr);
    images_.resize(count);
    vkGetSwapchainImagesKHR(device_->device(), swapchain_, &count, images_.data());
    return true;
}

bool VulkanSwapchain::create_image_views() {
    image_views_.resize(images_.size());
    for (size_t i = 0; i < images_.size(); ++i) {
        VkImageViewCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        info.image = images_[i];
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        info.format = format_;
        info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        info.subresourceRange.baseMipLevel = 0;
        info.subresourceRange.levelCount = 1;
        info.subresourceRange.baseArrayLayer = 0;
        info.subresourceRange.layerCount = 1;

        if (vkCreateImageView(device_->device(), &info, nullptr, &image_views_[i]) != VK_SUCCESS) {
            GLOG_ERROR("VulkanSwapchain: failed to create image view {}", i);
            return false;
        }
    }
    return true;
}

bool VulkanSwapchain::create_depth_attachment() {
    VkDevice dev = device_->device();
    depth_images_.resize(images_.size(), VK_NULL_HANDLE);
    depth_allocations_.resize(images_.size(), nullptr);
    depth_views_.resize(images_.size(), VK_NULL_HANDLE);

    for (size_t i = 0; i < images_.size(); ++i) {
        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.extent.width = extent_.width;
        info.extent.height = extent_.height;
        info.extent.depth = 1;
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.format = depth_format_;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo alloc_info{};
        alloc_info.usage = VMA_MEMORY_USAGE_GPU_ONLY;

        if (vmaCreateImage(device_->allocator(), &info, &alloc_info,
                           &depth_images_[i], &depth_allocations_[i], nullptr) != VK_SUCCESS) {
            GLOG_ERROR("VulkanSwapchain: failed to create depth image {}", i);
            return false;
        }

        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = depth_images_[i];
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = depth_format_;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        view_info.subresourceRange.baseMipLevel = 0;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.baseArrayLayer = 0;
        view_info.subresourceRange.layerCount = 1;
        if (vkCreateImageView(dev, &view_info, nullptr, &depth_views_[i]) != VK_SUCCESS) {
            GLOG_ERROR("VulkanSwapchain: failed to create depth view {}", i);
            return false;
        }
    }
    return true;
}

bool VulkanSwapchain::create_render_pass() {
    // 通用附件/子通道描述，按 loadOp 不同生成 clear 与 load 两个 render pass。
    // 注意：所有被 VkRenderPassCreateInfo 引用的局部数组必须存活到 vkCreateRenderPass 返回。
    VkAttachmentDescription color{};
    color.format = format_;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;  // 占位，创建前按需覆盖
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentDescription depth{};
    depth.format = depth_format_;
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;  // 占位
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference color_ref{};
    color_ref.attachment = 0;
    color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depth_ref{};
    depth_ref.attachment = 1;
    depth_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;
    subpass.pDepthStencilAttachment = &depth_ref;

    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                           VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    deps[0].srcAccessMask = 0;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                           VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    deps[1].dstAccessMask = 0;

    VkAttachmentDescription attachments[2] = {color, depth};

    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 2;
    info.pAttachments = attachments;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 2;
    info.pDependencies = deps;

    // 首屏/每帧开始：清除 color + depth
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    if (vkCreateRenderPass(device_->device(), &info, nullptr, &render_pass_) != VK_SUCCESS) {
        GLOG_ERROR("VulkanSwapchain: failed to create clear render pass");
        return false;
    }

    // 从 offscreen framebuffer 切回 swapchain 时保留已有内容
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    if (vkCreateRenderPass(device_->device(), &info, nullptr, &render_pass_load_) != VK_SUCCESS) {
        GLOG_ERROR("VulkanSwapchain: failed to create load render pass");
        return false;
    }
    return true;
}

bool VulkanSwapchain::create_framebuffers() {
    framebuffers_.resize(image_views_.size());
    for (size_t i = 0; i < image_views_.size(); ++i) {
        VkImageView attachments[] = {image_views_[i], depth_views_[i]};
        VkFramebufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        info.renderPass = render_pass_;
        info.attachmentCount = 2;
        info.pAttachments = attachments;
        info.width = extent_.width;
        info.height = extent_.height;
        info.layers = 1;

        if (vkCreateFramebuffer(device_->device(), &info, nullptr, &framebuffers_[i]) != VK_SUCCESS) {
            GLOG_ERROR("VulkanSwapchain: failed to create framebuffer {}", i);
            return false;
        }
    }
    return true;
}

bool VulkanSwapchain::create_command_buffer() {
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = device_->graphics_queue_family();

    if (vkCreateCommandPool(device_->device(), &pool_info, nullptr, &command_pool_) != VK_SUCCESS) {
        GLOG_ERROR("VulkanSwapchain: failed to create command pool");
        return false;
    }

    command_buffers_.resize(images_.size(), VK_NULL_HANDLE);
    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = command_pool_;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = static_cast<uint32_t>(command_buffers_.size());

    if (vkAllocateCommandBuffers(device_->device(), &alloc_info, command_buffers_.data()) != VK_SUCCESS) {
        GLOG_ERROR("VulkanSwapchain: failed to allocate command buffers");
        return false;
    }
    return true;
}

bool VulkanSwapchain::create_secondary_command_buffer() {
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = device_->graphics_queue_family();

    if (vkCreateCommandPool(device_->device(), &pool_info, nullptr, &secondary_pool_) != VK_SUCCESS) {
        GLOG_ERROR("VulkanSwapchain: failed to create secondary command pool");
        return false;
    }

    secondary_command_buffers_.resize(images_.size(), VK_NULL_HANDLE);
    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = secondary_pool_;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
    alloc_info.commandBufferCount = static_cast<uint32_t>(secondary_command_buffers_.size());

    if (vkAllocateCommandBuffers(device_->device(), &alloc_info, secondary_command_buffers_.data()) != VK_SUCCESS) {
        GLOG_ERROR("VulkanSwapchain: failed to allocate secondary command buffers");
        return false;
    }
    return true;
}

bool VulkanSwapchain::create_sync_objects() {
    VkSemaphoreCreateInfo sem_info{};
    sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    VkDevice dev = device_->device();
    frames_in_flight_ = static_cast<int>(std::min(images_.size(), static_cast<size_t>(kMaxFramesInFlight)));
    image_available_semaphores_.resize(frames_in_flight_);
    render_finished_semaphores_.resize(frames_in_flight_);
    frame_fences_.resize(frames_in_flight_, VK_NULL_HANDLE);

    for (int i = 0; i < frames_in_flight_; ++i) {
        if (vkCreateSemaphore(dev, &sem_info, nullptr, &image_available_semaphores_[i]) != VK_SUCCESS ||
            vkCreateSemaphore(dev, &sem_info, nullptr, &render_finished_semaphores_[i]) != VK_SUCCESS ||
            vkCreateFence(dev, &fence_info, nullptr, &frame_fences_[i]) != VK_SUCCESS) {
            GLOG_ERROR("VulkanSwapchain: failed to create sync objects");
            return false;
        }
    }
    GLOG_INFO("VulkanSwapchain: frames_in_flight={}", frames_in_flight_);
    return true;
}

VkResult VulkanSwapchain::acquire_next_image(uint32_t* image_index) {
    // 等待当前 frame 上一帧的 GPU 工作完成，避免信号量/UBO/命令缓冲被复用时还在使用。
    VkFence frame_fence = frame_fences_[current_frame_];
    vkWaitForFences(device_->device(), 1, &frame_fence, VK_TRUE, UINT64_MAX);
    vkResetFences(device_->device(), 1, &frame_fence);

    VkSemaphore signal_semaphore = image_available_semaphores_[current_frame_];
    VkResult result = vkAcquireNextImageKHR(device_->device(), swapchain_, UINT64_MAX,
                                            signal_semaphore, VK_NULL_HANDLE, image_index);
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        return result;
    }

    current_image_ = *image_index;
    return result;
}

VkResult VulkanSwapchain::present(uint32_t image_index, VkSemaphore wait_semaphore) {
    VkPresentInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &wait_semaphore;
    info.swapchainCount = 1;
    info.pSwapchains = &swapchain_;
    info.pImageIndices = &image_index;

    return vkQueuePresentKHR(device_->present_queue(), &info);
}

VkResult VulkanSwapchain::submit_and_present(uint32_t image_index, VkCommandBuffer cmd) {
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        return VK_ERROR_DEVICE_LOST;
    }

    VkSemaphore image_available = image_available_semaphores_[current_frame_];
    VkSemaphore render_finished = render_finished_semaphores_[current_frame_];
    VkFence fence = frame_fences_[current_frame_];

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &image_available;
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &render_finished;

    if (vkQueueSubmit(device_->graphics_queue(), 1, &submit, fence) != VK_SUCCESS) {
        return VK_ERROR_DEVICE_LOST;
    }

    VkResult result = present(image_index, render_finished);
    current_frame_ = (current_frame_ + 1) % frames_in_flight_;
    return result;
}

void VulkanSwapchain::advance_frame() {
    current_frame_ = (current_frame_ + 1) % frames_in_flight_;
}

} // namespace gryce_engine::render
