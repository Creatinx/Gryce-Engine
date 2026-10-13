#include "vk_framebuffer.h"

#include "vk_device.h"
#include "vk_swapchain.h"
#include "vk_texture.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::render {

// 采样数（int）→ VkSampleCountFlagBits。非法/非 2 幂值回落 1x，
// 由调用方的能力协商保证上限。
VkSampleCountFlagBits VulkanFramebuffer::to_vk_sample_count(int samples) {
    switch (samples) {
        case 2:  return VK_SAMPLE_COUNT_2_BIT;
        case 4:  return VK_SAMPLE_COUNT_4_BIT;
        case 8:  return VK_SAMPLE_COUNT_8_BIT;
        case 16: return VK_SAMPLE_COUNT_16_BIT;
        default: return VK_SAMPLE_COUNT_1_BIT;
    }
}

int VulkanFramebuffer::color_sample_count() const {
    return color_texture_ ? color_texture_->sample_count() : 1;
}

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
    resolve_texture_ = nullptr;
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
    if (resolve_texture_) {
        attach_resolve_texture(resolve_texture_);
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

void VulkanFramebuffer::attach_resolve_texture(ITexture* texture) {
    auto* vk_tex = dynamic_cast<VulkanTexture*>(texture);
    if (!vk_tex) {
        GLOG_ERROR("VulkanFramebuffer: resolve texture is not VulkanTexture");
        return;
    }
    resolve_texture_ = vk_tex;

    // 与颜色/深度附件同一模式：销毁旧 framebuffer / render_pass 后按完整附件重建。
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
    // 与 color_refs 平行：每项要么 VK_ATTACHMENT_UNUSED（不解析），
    // 要么指向 resolve 附件。无多样本颜色时保持为空。
    std::vector<VkAttachmentReference> resolve_refs;
    VkAttachmentReference depth_ref{};
    bool has_depth = (depth_texture_ != nullptr);
    bool has_color = (color_texture_ != nullptr);
    // 仅多样本颜色 + 有 resolve 目标时才走 pResolveAttachments
    // （VU 要求 resolve 附件必须单样本，且对应颜色附件采样数 > 1）。
    bool has_resolve = has_color && color_sample_count() > 1 && (resolve_texture_ != nullptr);

    if (has_color) {
        VkAttachmentDescription color{};
        color.format = color_texture_->format();
        color.samples = to_vk_sample_count(color_texture_->sample_count());
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        // 有 resolve 附件时多样本颜色内容在 pass 末尾被解析走，自身无需保留。
        color.storeOp = has_resolve ? VK_ATTACHMENT_STORE_OP_DONT_CARE
                                    : VK_ATTACHMENT_STORE_OP_STORE;
        color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        color.finalLayout = has_resolve ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                        : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        attachments.push_back(color);

        VkAttachmentReference ref{};
        ref.attachment = static_cast<uint32_t>(attachments.size() - 1);
        ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_refs.push_back(ref);

        VkAttachmentReference resolve_ref{};
        resolve_ref.attachment = VK_ATTACHMENT_UNUSED;
        resolve_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        if (has_resolve) {
            VkAttachmentDescription resolve{};
            resolve.format = resolve_texture_->format();
            resolve.samples = VK_SAMPLE_COUNT_1_BIT;
            resolve.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            resolve.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            resolve.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            resolve.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            resolve.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            resolve.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            attachments.push_back(resolve);
            resolve_ref.attachment = static_cast<uint32_t>(attachments.size() - 1);
        }
        resolve_refs.push_back(resolve_ref);
    }

    if (has_depth) {
        VkAttachmentDescription depth{};
        depth.format = depth_texture_->format();
        // VK 规范（无 mixed_attachment_samples 扩展）要求同一 subpass 的颜色/
        // 深度附件采样数一致（VUID-VkSubpassDescription-pDepthStencilAttachment-01418），
        // 所以多样本颜色附件必须搭配多样本深度。深度的事后解析由 depth resolve
        // pass（sampler2DMS 逐样本平均）完成，render pass 本身只 resolve 颜色。
        depth.samples = to_vk_sample_count(depth_texture_->sample_count());
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        // 多样本深度后续要被 depth resolve pass 当 SRV 采样 → 必须 STORE。
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
    // 有 resolve 附件时逐颜色附件给出解析目标；数组长度必须等于 colorAttachmentCount。
    subpass.pResolveAttachments = resolve_refs.empty() ? nullptr : resolve_refs.data();

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
    // 视图顺序必须与 create_render_pass 的附件索引严格一致：
    // 颜色(0) → resolve(1，仅多样本颜色时存在) → 深度(最后一个)。
    // 顺序颠倒会被 validation 判格式/usage 不匹配，驱动侧直接 device lost。
    if (color_texture_ && color_texture_->image_view()) {
        attachments.push_back(color_texture_->image_view());
    }
    const bool has_resolve =
        color_texture_ && color_sample_count() > 1 && resolve_texture_ != nullptr;
    if (has_resolve && resolve_texture_->image_view()) {
        attachments.push_back(resolve_texture_->image_view());
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
    if (cmd == VK_NULL_HANDLE) {
        GLOG_ERROR("VulkanFramebuffer::begin_render_pass: cmd is null");
        return;
    }
    if (render_pass_ == VK_NULL_HANDLE || framebuffer_ == VK_NULL_HANDLE) {
        GLOG_ERROR("VulkanFramebuffer::begin_render_pass: render_pass={} framebuffer={} width={} height={}",
                   reinterpret_cast<uintptr_t>(render_pass_),
                   reinterpret_cast<uintptr_t>(framebuffer_),
                   width_, height_);
        return;
    }
    if (width_ <= 0 || height_ <= 0) {
        GLOG_ERROR("VulkanFramebuffer::begin_render_pass: invalid extent {}x{}", width_, height_);
        return;
    }

    VkRenderPassBeginInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    info.renderPass = render_pass_;
    info.framebuffer = framebuffer_;
    info.renderArea.offset = {0, 0};
    info.renderArea.extent = {static_cast<uint32_t>(width_), static_cast<uint32_t>(height_)};

    std::vector<VkClearValue> clears;
    // ★ pClearValues 必须按 render pass 的**附件索引**顺序排列：
    //   颜色(0) → resolve(1，仅多样本颜色) → 深度(最后)。
    // 顺序错位会让 depth 的 CLEAR 拿到 resolve 位的零值（深度被清成 0.0，
    // 整个场景深度测试全灭）。resolve 附件 loadOp 为 DONT_CARE，值被忽略，
    // 但占位必须存在。
    if (color_texture_) {
        VkClearValue color_clear{};
        color_clear.color = {{clear_color_r_, clear_color_g_, clear_color_b_, clear_color_a_}};
        clears.push_back(color_clear);
    }
    if (resolve_texture_ && color_sample_count() > 1) {
        VkClearValue resolve_clear{};
        clears.push_back(resolve_clear);
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
