#include "vk_framebuffer.h"

#include "vk_device.h"
#include "vk_swapchain.h"
#include "vk_texture.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::render {

VulkanFramebuffer::VulkanFramebuffer(VulkanDevice* device, VulkanSwapchain* swapchain)
    : device_(device), swapchain_(swapchain) {}

VulkanFramebuffer::~VulkanFramebuffer() {
    destroy();
}

bool VulkanFramebuffer::create(int width, int height) {
    width_ = width;
    height_ = height;
    return create_render_pass();
}

void VulkanFramebuffer::destroy() {
    if (device_ && device_->is_valid()) {
        VkDevice dev = device_->device();
        if (framebuffer_) vkDestroyFramebuffer(dev, framebuffer_, nullptr);
        if (render_pass_) vkDestroyRenderPass(dev, render_pass_, nullptr);
    }
    if (owns_depth_texture_ && depth_texture_) {
        delete depth_texture_;
    }
    framebuffer_ = VK_NULL_HANDLE;
    render_pass_ = VK_NULL_HANDLE;
    color_texture_ = nullptr;
    depth_texture_ = nullptr;
    owns_depth_texture_ = false;
}

void VulkanFramebuffer::resize(int width, int height) {
    destroy();
    create(width, height);
    if (color_texture_) {
        attach_color_texture(color_texture_);
    }
    if (depth_texture_) {
        attach_depth_texture(depth_texture_);
    }
}

void VulkanFramebuffer::attach_color_texture(ITexture* texture) {
    auto* vk_tex = dynamic_cast<VulkanTexture*>(texture);
    if (!vk_tex) {
        GLOG_ERROR("VulkanFramebuffer: color texture is not VulkanTexture");
        return;
    }
    color_texture_ = vk_tex;

    // 销毁旧的 framebuffer / render_pass，再重新根据完整附件创建。
    if (framebuffer_ && device_ && device_->is_valid()) {
        vkDestroyFramebuffer(device_->device(), framebuffer_, nullptr);
        framebuffer_ = VK_NULL_HANDLE;
    }
    VkRenderPass old_render_pass = render_pass_;
    if (!create_render_pass()) return;
    if (old_render_pass && device_ && device_->is_valid()) {
        vkDestroyRenderPass(device_->device(), old_render_pass, nullptr);
    }
    if (!create_framebuffer()) {
        if (render_pass_ && device_ && device_->is_valid()) {
            vkDestroyRenderPass(device_->device(), render_pass_, nullptr);
        }
        render_pass_ = VK_NULL_HANDLE;
    }
}

void VulkanFramebuffer::attach_depth_texture(ITexture* texture) {
    auto* vk_tex = dynamic_cast<VulkanTexture*>(texture);
    if (!vk_tex) {
        GLOG_ERROR("VulkanFramebuffer: depth texture is not VulkanTexture");
        return;
    }
    if (depth_texture_ && owns_depth_texture_) {
        delete depth_texture_;
    }
    depth_texture_ = vk_tex;
    owns_depth_texture_ = false;

    // 销毁旧的 framebuffer / render_pass，再重新根据完整附件创建。
    if (framebuffer_ && device_ && device_->is_valid()) {
        vkDestroyFramebuffer(device_->device(), framebuffer_, nullptr);
        framebuffer_ = VK_NULL_HANDLE;
    }
    VkRenderPass old_render_pass = render_pass_;
    if (!create_render_pass()) return;
    if (old_render_pass && device_ && device_->is_valid()) {
        vkDestroyRenderPass(device_->device(), old_render_pass, nullptr);
    }
    if (!create_framebuffer()) {
        if (render_pass_ && device_ && device_->is_valid()) {
            vkDestroyRenderPass(device_->device(), render_pass_, nullptr);
        }
        render_pass_ = VK_NULL_HANDLE;
    }
}

bool VulkanFramebuffer::create_render_pass() {
    std::vector<VkAttachmentDescription> attachments;
    std::vector<VkAttachmentReference> color_refs;
    VkAttachmentReference depth_ref{};
    bool has_depth = (depth_texture_ != nullptr);
    bool has_color = (color_texture_ != nullptr);

    if (has_color) {
        VkAttachmentDescription color{};
        color.format = color_texture_->format();
        color.samples = VK_SAMPLE_COUNT_1_BIT;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        color.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        attachments.push_back(color);

        VkAttachmentReference ref{};
        ref.attachment = static_cast<uint32_t>(attachments.size() - 1);
        ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_refs.push_back(ref);
    }

    if (has_depth) {
        VkAttachmentDescription depth{};
        depth.format = depth_texture_->format();
        depth.samples = VK_SAMPLE_COUNT_1_BIT;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        attachments.push_back(depth);

        depth_ref.attachment = static_cast<uint32_t>(attachments.size() - 1);
        depth_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = static_cast<uint32_t>(color_refs.size());
    subpass.pColorAttachments = color_refs.empty() ? nullptr : color_refs.data();
    subpass.pDepthStencilAttachment = has_depth ? &depth_ref : nullptr;

    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[0].dstStageMask = has_color
        ? (VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT)
        : VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    deps[0].dstAccessMask = has_color
        ? (VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)
        : VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = has_color
        ? (VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT)
        : VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[1].srcAccessMask = has_color
        ? (VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)
        : VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = static_cast<uint32_t>(attachments.size());
    info.pAttachments = attachments.data();
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 2;
    info.pDependencies = deps;

    if (vkCreateRenderPass(device_->device(), &info, nullptr, &render_pass_) != VK_SUCCESS) {
        GLOG_ERROR("VulkanFramebuffer: failed to create render pass");
        return false;
    }
    return true;
}

bool VulkanFramebuffer::create_framebuffer() {
    std::vector<VkImageView> attachments;
    if (color_texture_ && color_texture_->image_view()) {
        attachments.push_back(color_texture_->image_view());
    }
    if (depth_texture_ && depth_texture_->image_view()) {
        attachments.push_back(depth_texture_->image_view());
    }
    if (attachments.empty()) {
        GLOG_ERROR("VulkanFramebuffer: no attachments for framebuffer");
        return false;
    }

    VkFramebufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    info.renderPass = render_pass_;
    info.attachmentCount = static_cast<uint32_t>(attachments.size());
    info.pAttachments = attachments.data();
    info.width = static_cast<uint32_t>(width_);
    info.height = static_cast<uint32_t>(height_);
    info.layers = 1;

    if (vkCreateFramebuffer(device_->device(), &info, nullptr, &framebuffer_) != VK_SUCCESS) {
        GLOG_ERROR("VulkanFramebuffer: failed to create framebuffer");
        return false;
    }
    return true;
}

void VulkanFramebuffer::set_clear_color(float r, float g, float b, float a) {
    clear_color_r_ = r;
    clear_color_g_ = g;
    clear_color_b_ = b;
    clear_color_a_ = a;
}

void VulkanFramebuffer::begin_render_pass(VkCommandBuffer cmd, VkSubpassContents contents) const {
    if (!render_pass_ || !framebuffer_) return;

    VkRenderPassBeginInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    info.renderPass = render_pass_;
    info.framebuffer = framebuffer_;
    info.renderArea.offset = {0, 0};
    info.renderArea.extent = {static_cast<uint32_t>(width_), static_cast<uint32_t>(height_)};

    std::vector<VkClearValue> clears;
    if (color_texture_) {
        VkClearValue color_clear{};
        color_clear.color = {{clear_color_r_, clear_color_g_, clear_color_b_, clear_color_a_}};
        clears.push_back(color_clear);
    }
    if (depth_texture_) {
        VkClearValue depth_clear{};
        depth_clear.depthStencil = {1.0f, 0};
        clears.push_back(depth_clear);
    }

    info.clearValueCount = static_cast<uint32_t>(clears.size());
    info.pClearValues = clears.data();

    vkCmdBeginRenderPass(cmd, &info, contents);
}

void VulkanFramebuffer::end_render_pass(VkCommandBuffer cmd) const {
    vkCmdEndRenderPass(cmd);
}

} // namespace gryce_engine::render
