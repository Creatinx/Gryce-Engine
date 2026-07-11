#include "vk_backend.h"

#include <cstring>
#include <fstream>
#include <vector>

#include <GLFW/glfw3.h>

#include "render/mesh.h"
#include "render/shader.h"
#include "render/texture.h"
#include "render/framebuffer.h"
#include "utils/glog/glog_lib.h"
#include "vk_mesh.h"
#include "vk_shader.h"
#include "vk_texture.h"
#include "vk_framebuffer.h"
#include "vk_renderer2d.h"
#include "vk_imgui_backend.h"
#include "vk_instance.h"

namespace gryce_engine::render {

namespace {

class VulkanMeshStub : public IMesh {
public:
    void upload_vertices(const void* /*data*/, uint32_t /*size*/, uint32_t /*count*/) override {}
    void upload_indices(const void* /*data*/, uint32_t /*size*/, uint32_t /*count*/) override {}
    void set_layout(const VertexLayout& /*layout*/) override {}
    void bind() const override {}
    void draw() const override {}
    void draw_indexed() const override {}
    uint32_t vertex_count() const override { return 0; }
    uint32_t index_count() const override { return 0; }
};

class VulkanShaderStub : public IShader {
public:
    bool compile(const std::string& /*vertex_src*/, const std::string& /*fragment_src*/) override { return false; }
    bool compile(const std::vector<ShaderStageDesc>& /*stages*/) override { return false; }
    void bind() const override {}
    void unbind() const override {}
    void set_int(const std::string& /*name*/, int /*value*/) override {}
    void set_int(const char* /*name*/, int /*value*/) override {}
    void set_float(const std::string& /*name*/, float /*value*/) override {}
    void set_float(const char* /*name*/, float /*value*/) override {}
    void set_vec2(const std::string& /*name*/, const math::Vector2f& /*value*/) override {}
    void set_vec2(const char* /*name*/, const math::Vector2f& /*value*/) override {}
    void set_vec3(const std::string& /*name*/, const math::Vector3f& /*value*/) override {}
    void set_vec3(const char* /*name*/, const math::Vector3f& /*value*/) override {}
    void set_vec4(const std::string& /*name*/, const math::Vector4f& /*value*/) override {}
    void set_vec4(const char* /*name*/, const math::Vector4f& /*value*/) override {}
    void set_mat4(const std::string& /*name*/, const math::Matrix4f& /*value*/) override {}
    void set_mat4(const char* /*name*/, const math::Matrix4f& /*value*/) override {}
    bool is_valid() const override { return false; }
};

class VulkanTextureStub : public ITexture {
public:
    bool load_from_file(const std::string& /*path*/) override { return false; }
    bool create_empty(int /*width*/, int /*height*/, int /*channels*/) override { return false; }
    bool upload_data(const void* /*data*/, int /*width*/, int /*height*/, int /*channels*/) override { return false; }
    bool create_depth(int /*width*/, int /*height*/) override { return false; }
    bool create(TextureFormat /*format*/, int /*width*/, int /*height*/, const void* /*data*/) override { return false; }
    void bind(uint32_t /*slot*/) const override {}
    void unbind() const override {}
    void set_filter(TextureFilter /*min*/, TextureFilter /*mag*/) override {}
    void set_wrap(TextureWrap /*s*/, TextureWrap /*t*/) override {}
    int width() const override { return 0; }
    int height() const override { return 0; }
    bool is_valid() const override { return false; }
};

} // namespace

VulkanBackend::VulkanBackend() = default;

VulkanBackend::~VulkanBackend() {
    shutdown();
}

bool VulkanBackend::init(void* native_window) {
    window_ = static_cast<GLFWwindow*>(native_window);
    if (!window_) {
        GLOG_ERROR("VulkanBackend::init: null window");
        return false;
    }

    uint32_t glfw_ext_count = 0;
    const char** glfw_exts = glfwGetRequiredInstanceExtensions(&glfw_ext_count);
    std::vector<const char*> extensions(glfw_exts, glfw_exts + glfw_ext_count);

    if (!instance_.init(extensions)) {
        return false;
    }

    const char* glfw_err = nullptr;
    glfwGetError(&glfw_err);
    VkResult surface_result = glfwCreateWindowSurface(instance_.handle(), window_, nullptr, &surface_);
    if (surface_result != VK_SUCCESS) {
        glfwGetError(&glfw_err);
        GLOG_ERROR("VulkanBackend::init: failed to create window surface, VkResult={} glfw='{}'",
                   static_cast<int>(surface_result), glfw_err ? glfw_err : "none");
        return false;
    }

    if (!device_.init(instance_.handle(), surface_)) {
        return false;
    }

    int width = 0, height = 0;
    glfwGetFramebufferSize(window_, &width, &height);
    if (!swapchain_.init(instance_.handle(), &device_, surface_,
                         static_cast<uint32_t>(width), static_cast<uint32_t>(height))) {
        return false;
    }

    if (!load_dynamic_state_functions()) {
        GLOG_WARN("VulkanBackend: extended dynamic state not available, "
                  "set_cull_face/set_depth_test/set_blend will be static pipeline defaults");
    }

    initialized_ = true;
    GLOG_INFO("VulkanBackend initialized");
    return true;
}

bool VulkanBackend::load_dynamic_state_functions() {
    if (!device_.supports_extended_dynamic_state()) return false;

    vk_cmd_set_cull_mode_ = reinterpret_cast<PFN_vkCmdSetCullModeEXT>(
        vkGetDeviceProcAddr(device_.device(), "vkCmdSetCullModeEXT"));
    vk_cmd_set_front_face_ = reinterpret_cast<PFN_vkCmdSetFrontFaceEXT>(
        vkGetDeviceProcAddr(device_.device(), "vkCmdSetFrontFaceEXT"));
    vk_cmd_set_depth_test_enable_ = reinterpret_cast<PFN_vkCmdSetDepthTestEnableEXT>(
        vkGetDeviceProcAddr(device_.device(), "vkCmdSetDepthTestEnableEXT"));
    vk_cmd_set_depth_write_enable_ = reinterpret_cast<PFN_vkCmdSetDepthWriteEnableEXT>(
        vkGetDeviceProcAddr(device_.device(), "vkCmdSetDepthWriteEnableEXT"));

    supports_dynamic_state_ = vk_cmd_set_cull_mode_ && vk_cmd_set_front_face_ &&
                              vk_cmd_set_depth_test_enable_ && vk_cmd_set_depth_write_enable_;
    return supports_dynamic_state_;
}

void VulkanBackend::shutdown() {
    if (!initialized_) return;

    if (device_.device()) {
        vkDeviceWaitIdle(device_.device());
    }

    swapchain_.shutdown();
    device_.shutdown();

    if (surface_ != VK_NULL_HANDLE && instance_.handle()) {
        vkDestroySurfaceKHR(instance_.handle(), surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }

    instance_.shutdown();
    window_ = nullptr;
    initialized_ = false;
}

void VulkanBackend::make_current(void* /*native_window*/) {
    // Vulkan 不需要像 GL 那样 make current
}

void VulkanBackend::release_context() {
    // Vulkan 不需要释放 context
}

void VulkanBackend::flush_gpu() {
    // Vulkan 命令通过 submit 自动 flush；queue wait 可作为可选同步点。
    if (initialized_ && device_.graphics_queue()) {
        vkQueueWaitIdle(device_.graphics_queue());
    }
}

void VulkanBackend::wait_gpu_idle() {
    if (initialized_ && device_.device()) {
        vkDeviceWaitIdle(device_.device());
    }
}

void VulkanBackend::set_swap_interval(int interval) {
    if (!initialized_) return;
    bool enabled = interval != 0;
    if (swapchain_.vsync_enabled() != enabled) {
        swapchain_.set_vsync_enabled(enabled);
    }
}

void VulkanBackend::begin_frame() {
    if (!initialized_) return;

    // 窗口大小变化时重建 swapchain
    int width = 0, height = 0;
    glfwGetFramebufferSize(window_, &width, &height);
    if (width > 0 && height > 0) {
        VkExtent2D extent = swapchain_.extent();
        if (static_cast<uint32_t>(width) != extent.width ||
            static_cast<uint32_t>(height) != extent.height) {
            if (!swapchain_.recreate(static_cast<uint32_t>(width), static_cast<uint32_t>(height))) {
                GLOG_ERROR("VulkanBackend: failed to recreate swapchain");
                return;
            }
            GLOG_INFO("VulkanBackend: swapchain recreated to {}x{}", width, height);
        }
    }

    VkResult acquire_result = swapchain_.acquire_next_image(&current_image_);
    if (acquire_result == VK_ERROR_OUT_OF_DATE_KHR) {
        if (width > 0 && height > 0) {
            swapchain_.recreate(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
            acquire_result = swapchain_.acquire_next_image(&current_image_);
        }
    }
    if (acquire_result != VK_SUCCESS && acquire_result != VK_SUBOPTIMAL_KHR) {
        GLOG_ERROR("VulkanBackend: failed to acquire swapchain image");
        return;
    }

    reset_state_cache();
    if (device_.device() != VK_NULL_HANDLE && !per_frame_secondary_cbs_.empty()) {
        vkFreeCommandBuffers(device_.device(), swapchain_.secondary_command_pool(),
                             static_cast<uint32_t>(per_frame_secondary_cbs_.size()),
                             per_frame_secondary_cbs_.data());
    }
    per_frame_secondary_cbs_.clear();
    inline_secondary_cb_ = VK_NULL_HANDLE;
    inline_secondary_recording_ = false;
    current_render_pass_ = VK_NULL_HANDLE;
    current_framebuffer_vk_ = VK_NULL_HANDLE;
    render_pass_contents_secondary_ = false;

    VkCommandBuffer primary = primary_command_buffer();
    vkResetCommandBuffer(primary, 0);

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(primary, &begin_info);

    VkClearValue clears[2]{};
    clears[0].color = {{clear_r_, clear_g_, clear_b_, clear_a_}};
    clears[1].depthStencil = {1.0f, 0};
    begin_render_pass_secondary(swapchain_.render_pass(), swapchain_.framebuffer(current_image_),
                                clears, 2, swapchain_.extent());
    in_forward_pass_ = true;

    // 默认 viewport / scissor 记录到 inline secondary CB
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = static_cast<float>(swapchain_.extent().height);
    viewport.width = static_cast<float>(swapchain_.extent().width);
    viewport.height = -static_cast<float>(swapchain_.extent().height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    set_viewport_cached(current_command_buffer(), viewport);

    VkRect2D scissor{};
    scissor.extent = swapchain_.extent();
    set_scissor_cached(current_command_buffer(), scissor);
}

void VulkanBackend::end_frame() {
    if (!initialized_) return;
    end_and_execute_inline_secondary();
    end_current_render_pass();
    in_forward_pass_ = false;

    bool need_screenshot = (!screenshot_path_.empty() && frame_count_ == screenshot_frame_);

    VkResult present_result = VK_SUCCESS;
    if (need_screenshot) {
        // 先提交渲染命令并等待完成，再截图，最后 present
        VkCommandBuffer primary = primary_command_buffer();
        if (vkEndCommandBuffer(primary) == VK_SUCCESS) {
            VkSemaphore image_available = swapchain_.current_image_available_semaphore();
            VkSemaphore render_finished = swapchain_.current_render_finished_semaphore();
            VkFence fence = swapchain_.current_fence();

            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            submit.waitSemaphoreCount = 1;
            submit.pWaitSemaphores = &image_available;
            submit.pWaitDstStageMask = &wait_stage;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &primary;
            submit.signalSemaphoreCount = 1;
            submit.pSignalSemaphores = &render_finished;
            vkQueueSubmit(device_.graphics_queue(), 1, &submit, fence);
            vkWaitForFences(device_.device(), 1, &fence, VK_TRUE, UINT64_MAX);

            save_screenshot(screenshot_path_);
            screenshot_path_.clear();
        }
        present_result = swapchain_.present(current_image_, swapchain_.current_render_finished_semaphore());
        swapchain_.advance_frame();
    } else {
        present_result = swapchain_.submit_and_present(current_image_, primary_command_buffer());
    }

    if (present_result == VK_ERROR_OUT_OF_DATE_KHR || present_result == VK_SUBOPTIMAL_KHR) {
        int width = 0, height = 0;
        glfwGetFramebufferSize(window_, &width, &height);
        if (width > 0 && height > 0) {
            swapchain_.recreate(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
        }
    }

    ++frame_count_;
}

void VulkanBackend::clear(float r, float g, float b, float a) {
    clear_r_ = r;
    clear_g_ = g;
    clear_b_ = b;
    clear_a_ = a;
}

uint32_t VulkanBackend::max_viewports() const {
    return k_max_viewports;
}

void VulkanBackend::set_viewport(int x, int y, int w, int h) {
    set_viewport(x, y, w, h, 0);
}

void VulkanBackend::set_scissor(int x, int y, int w, int h) {
    set_scissor(x, y, w, h, 0);
}

void VulkanBackend::set_viewport(int x, int y, int w, int h, uint32_t viewport_index) {
    if (!initialized_ || viewport_index >= k_max_viewports) return;
    VkCommandBuffer cmd = current_command_buffer();
    if (cmd == VK_NULL_HANDLE) return;
    VkViewport viewport{};
    viewport.x = static_cast<float>(x);
    viewport.y = static_cast<float>(y + h);
    viewport.width = static_cast<float>(w);
    viewport.height = -static_cast<float>(h);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    set_viewport_cached(cmd, viewport, viewport_index);
}

void VulkanBackend::set_scissor(int x, int y, int w, int h, uint32_t viewport_index) {
    if (!initialized_ || viewport_index >= k_max_viewports) return;
    VkCommandBuffer cmd = current_command_buffer();
    if (cmd == VK_NULL_HANDLE) return;
    VkRect2D scissor{};
    scissor.offset = {x, y};
    scissor.extent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
    set_scissor_cached(cmd, scissor, viewport_index);
}

void VulkanBackend::set_depth_test(bool enabled) {
    depth_test_enabled_ = enabled;
}

void VulkanBackend::set_blend(bool enabled) {
    blend_enabled_ = enabled;
}

void VulkanBackend::set_blend_func(BlendFactor src_factor, BlendFactor dst_factor) {
    // Vulkan 传统上在 pipeline create info 中固定混合因子；
    // 若后续启用 VK_EXT_extended_dynamic_state3，可在此调用 vkCmdSetColorBlendEquationEXT。
    // 目前仅记录期望值，供未来 pipeline 重建或动态状态使用。
    blend_src_factor_ = src_factor;
    blend_dst_factor_ = dst_factor;
}

void VulkanBackend::set_blend_equation(BlendEquation mode) {
    blend_equation_ = mode;
}

void VulkanBackend::set_cull_face(bool enabled) {
    cull_face_enabled_ = enabled;
}

void VulkanBackend::apply_dynamic_state(VkCommandBuffer cmd) {
    if (!supports_dynamic_state_ || cmd == VK_NULL_HANDLE) return;
    // Negative viewport height restores OpenGL's Y convention, so keep OpenGL winding.
    set_cull_mode_cached(cmd, cull_face_enabled_ ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE);
    set_front_face_cached(cmd, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    set_depth_test_cached(cmd, depth_test_enabled_ ? VK_TRUE : VK_FALSE);
    set_depth_write_cached(cmd, depth_test_enabled_ ? VK_TRUE : VK_FALSE);
}

void VulkanBackend::set_dynamic_state_2d(VkCommandBuffer cmd) {
    if (!supports_dynamic_state_ || cmd == VK_NULL_HANDLE) return;
    set_cull_mode_cached(cmd, VK_CULL_MODE_NONE);
    set_depth_test_cached(cmd, VK_FALSE);
    set_depth_write_cached(cmd, VK_FALSE);
}

void VulkanBackend::reset_state_cache() {
    state_cache_.pipeline = VK_NULL_HANDLE;
    state_cache_.pipeline_layout = VK_NULL_HANDLE;
    state_cache_.descriptor_set = VK_NULL_HANDLE;
    state_cache_.cull_mode = VK_CULL_MODE_FLAG_BITS_MAX_ENUM;
    state_cache_.front_face = VK_FRONT_FACE_CLOCKWISE;
    state_cache_.depth_test = VK_FALSE;
    state_cache_.depth_write = VK_FALSE;

    viewports_.fill(VkViewport{});
    scissors_.fill(VkRect2D{});
    viewport_count_ = 0;
    scissor_count_ = 0;
    applied_viewport_count_ = 0;
    applied_scissor_count_ = 0;
}

VkCommandBuffer VulkanBackend::primary_command_buffer() const {
    if (!initialized_) return VK_NULL_HANDLE;
    return swapchain_.command_buffer(current_image_);
}

VkCommandBuffer VulkanBackend::current_command_buffer() {
    if (!initialized_) return VK_NULL_HANDLE;
    if (!inline_secondary_recording_) {
        begin_inline_secondary();
    }
    return inline_secondary_cb_;
}

void VulkanBackend::begin_inline_secondary() {
    if (!initialized_ || inline_secondary_recording_) return;
    inline_secondary_cb_ = swapchain_.secondary_command_buffer(current_image_);
    if (inline_secondary_cb_ == VK_NULL_HANDLE) return;

    vkResetCommandBuffer(inline_secondary_cb_, 0);

    VkCommandBufferInheritanceInfo inheritance{};
    inheritance.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
    inheritance.renderPass = current_render_pass_;
    inheritance.subpass = 0;
    inheritance.framebuffer = current_framebuffer_vk_;

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT |
                       VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
    begin_info.pInheritanceInfo = &inheritance;

    vkBeginCommandBuffer(inline_secondary_cb_, &begin_info);
    inline_secondary_recording_ = true;
}

void VulkanBackend::end_inline_secondary() {
    if (!inline_secondary_recording_ || inline_secondary_cb_ == VK_NULL_HANDLE) return;
    vkEndCommandBuffer(inline_secondary_cb_);
    inline_secondary_recording_ = false;
}

void VulkanBackend::execute_inline_secondary() {
    VkCommandBuffer primary = primary_command_buffer();
    if (primary == VK_NULL_HANDLE || inline_secondary_cb_ == VK_NULL_HANDLE) return;
    if (!render_pass_contents_secondary_) return;
    vkCmdExecuteCommands(primary, 1, &inline_secondary_cb_);
    inline_secondary_cb_ = VK_NULL_HANDLE;
}

void VulkanBackend::end_and_execute_inline_secondary() {
    end_inline_secondary();
    execute_inline_secondary();
    // 主 CB 执行 secondary 后，其状态被隐式重置
    reset_state_cache();
}

VkCommandBuffer VulkanBackend::allocate_secondary_cb() {
    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = swapchain_.secondary_command_pool();
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
    alloc_info.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device_.device(), &alloc_info, &cb);
    if (cb != VK_NULL_HANDLE) {
        per_frame_secondary_cbs_.push_back(cb);
    }
    return cb;
}

void VulkanBackend::free_secondary_cb(VkCommandBuffer cb) {
    if (cb == VK_NULL_HANDLE || device_.device() == VK_NULL_HANDLE) return;
    auto it = std::find(per_frame_secondary_cbs_.begin(), per_frame_secondary_cbs_.end(), cb);
    if (it != per_frame_secondary_cbs_.end()) {
        per_frame_secondary_cbs_.erase(it);
    }
    vkFreeCommandBuffers(device_.device(), swapchain_.secondary_command_pool(), 1, &cb);
}

void VulkanBackend::reset_inline_secondary_state() {
    inline_secondary_cb_ = VK_NULL_HANDLE;
    inline_secondary_recording_ = false;
}

void VulkanBackend::execute_secondary(VkCommandBuffer secondary) {
    VkCommandBuffer primary = primary_command_buffer();
    if (primary == VK_NULL_HANDLE || secondary == VK_NULL_HANDLE) return;
    if (!render_pass_contents_secondary_) return;
    vkCmdExecuteCommands(primary, 1, &secondary);
}

void VulkanBackend::begin_render_pass_secondary(VkRenderPass rp, VkFramebuffer fb,
                                                const VkClearValue* clears, uint32_t clear_count,
                                                const VkExtent2D& extent) {
    VkCommandBuffer primary = primary_command_buffer();
    if (primary == VK_NULL_HANDLE) return;

    VkRenderPassBeginInfo rp_info{};
    rp_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp_info.renderPass = rp;
    rp_info.framebuffer = fb;
    rp_info.renderArea.offset = {0, 0};
    rp_info.renderArea.extent = extent;
    rp_info.clearValueCount = clear_count;
    rp_info.pClearValues = clears;

    vkCmdBeginRenderPass(primary, &rp_info, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
    current_render_pass_ = rp;
    current_framebuffer_vk_ = fb;
    render_pass_contents_secondary_ = true;
}

void VulkanBackend::end_current_render_pass() {
    VkCommandBuffer primary = primary_command_buffer();
    if (primary == VK_NULL_HANDLE) return;
    if (render_pass_contents_secondary_) {
        vkCmdEndRenderPass(primary);
        render_pass_contents_secondary_ = false;
    }
}

void VulkanBackend::bind_pipeline(VkCommandBuffer cmd, VkPipeline pipeline) {
    if (pipeline != VK_NULL_HANDLE && state_cache_.pipeline != pipeline) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        state_cache_.pipeline = pipeline;
    }
}

void VulkanBackend::bind_descriptor_set(VkCommandBuffer cmd, VkPipelineLayout layout, VkDescriptorSet set) {
    if (set != VK_NULL_HANDLE &&
        (state_cache_.pipeline_layout != layout || state_cache_.descriptor_set != set)) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
        state_cache_.pipeline_layout = layout;
        state_cache_.descriptor_set = set;
    }
}

void VulkanBackend::set_viewport_cached(VkCommandBuffer cmd, const VkViewport& viewport, uint32_t index) {
    if (index >= k_max_viewports || cmd == VK_NULL_HANDLE) return;
    viewports_[index] = viewport;
    if (index >= viewport_count_) viewport_count_ = index + 1;
    if (viewport_count_ != applied_viewport_count_) {
        vkCmdSetViewport(cmd, 0, viewport_count_, viewports_.data());
        applied_viewport_count_ = viewport_count_;
    }
}

void VulkanBackend::set_scissor_cached(VkCommandBuffer cmd, const VkRect2D& scissor, uint32_t index) {
    if (index >= k_max_viewports || cmd == VK_NULL_HANDLE) return;
    scissors_[index] = scissor;
    if (index >= scissor_count_) scissor_count_ = index + 1;
    if (scissor_count_ != applied_scissor_count_) {
        vkCmdSetScissor(cmd, 0, scissor_count_, scissors_.data());
        applied_scissor_count_ = scissor_count_;
    }
}

void VulkanBackend::set_cull_mode_cached(VkCommandBuffer cmd, VkCullModeFlags mode) {
    if (state_cache_.cull_mode != mode) {
        vk_cmd_set_cull_mode_(cmd, mode);
        state_cache_.cull_mode = mode;
    }
}

void VulkanBackend::set_front_face_cached(VkCommandBuffer cmd, VkFrontFace face) {
    if (state_cache_.front_face != face) {
        vk_cmd_set_front_face_(cmd, face);
        state_cache_.front_face = face;
    }
}

void VulkanBackend::set_depth_test_cached(VkCommandBuffer cmd, VkBool32 enable) {
    if (state_cache_.depth_test != enable) {
        vk_cmd_set_depth_test_enable_(cmd, enable);
        state_cache_.depth_test = enable;
    }
}

void VulkanBackend::set_depth_write_cached(VkCommandBuffer cmd, VkBool32 enable) {
    if (state_cache_.depth_write != enable) {
        vk_cmd_set_depth_write_enable_(cmd, enable);
        state_cache_.depth_write = enable;
    }
}

void VulkanBackend::bind_framebuffer(RHIFramebufferHandle fb) {
    if (!initialized_) return;
    auto* vk_fb = static_cast<VulkanFramebuffer*>(framebuffer(fb));
    if (!vk_fb) {
        unbind_framebuffer();
        return;
    }

    end_and_execute_inline_secondary();
    end_current_render_pass();
    in_forward_pass_ = false;

    // 使用 VulkanFramebuffer 自身的 clear 逻辑，但指定 secondary contents
    vk_fb->begin_render_pass(primary_command_buffer(), VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
    current_render_pass_ = vk_fb->render_pass();
    current_framebuffer_vk_ = vk_fb->framebuffer();
    render_pass_contents_secondary_ = true;
    current_framebuffer_ = fb;
}

void VulkanBackend::unbind_framebuffer() {
    if (!initialized_) return;
    if (in_forward_pass_) return;

    end_and_execute_inline_secondary();
    end_current_render_pass();
    if (current_framebuffer_.is_valid()) {
        auto* vk_fb = static_cast<VulkanFramebuffer*>(framebuffer(current_framebuffer_));
        if (vk_fb) {
            auto* depth_tex = vk_fb->depth_texture();
            if (depth_tex && depth_tex->is_depth()) {
                // shadow render pass 的 finalLayout 已完成过渡到 DEPTH_STENCIL_READ_ONLY_OPTIMAL
                depth_tex->set_layout(VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
            }
        }
        current_framebuffer_ = RHIFramebufferHandle{};
    }
    in_forward_pass_ = true;

    VkClearValue clears[2]{};
    clears[0].color = {{clear_r_, clear_g_, clear_b_, clear_a_}};
    clears[1].depthStencil = {1.0f, 0};
    begin_render_pass_secondary(swapchain_.render_pass(), swapchain_.framebuffer(current_image_),
                                clears, 2, swapchain_.extent());
}

void VulkanBackend::draw_mesh(RHIMeshHandle mesh, RHIShaderHandle shader) {
    auto* vk_mesh = static_cast<VulkanMesh*>(this->mesh(mesh));
    auto* vk_shader = static_cast<VulkanShader*>(this->shader(shader));
    if (!vk_mesh || !vk_shader || !vk_mesh->vertex_buffer()) return;

    // 结束并执行当前的 inline secondary CB，避免 draw_mesh 与 2D/ImGui 命令交错
    end_and_execute_inline_secondary();

    // 为本次 draw_mesh 分配并录制 secondary command buffer
    VkCommandBuffer secondary = allocate_secondary_cb();
    if (secondary == VK_NULL_HANDLE) return;

    VkCommandBufferInheritanceInfo inheritance{};
    inheritance.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
    inheritance.renderPass = current_render_pass_;
    inheritance.subpass = 0;
    inheritance.framebuffer = current_framebuffer_vk_;

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT |
                       VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
    begin_info.pInheritanceInfo = &inheritance;
    vkBeginCommandBuffer(secondary, &begin_info);

    // Secondary CB 必须显式设置动态 viewport/scissor
    if (applied_viewport_count_ > 0) {
        vkCmdSetViewport(secondary, 0, applied_viewport_count_, viewports_.data());
    }
    if (applied_scissor_count_ > 0) {
        vkCmdSetScissor(secondary, 0, applied_scissor_count_, scissors_.data());
    }

    bind_pipeline(secondary, vk_shader->pipeline());
    // Post-process shader 的 pipeline 未声明动态 cull/depth 状态，跳过 dynamic state。
    if (!vk_shader->is_post_process()) {
        apply_dynamic_state(secondary);
    }
    bind_descriptor_set(secondary, vk_shader->layout(), vk_shader->descriptor_set());
    vk_shader->push_constants(secondary);
    vk_shader->update_ubo(secondary);

    VkBuffer buffers[] = {vk_mesh->vertex_buffer()};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(secondary, 0, 1, buffers, offsets);

    if (vk_mesh->has_index() && vk_mesh->index_buffer()) {
        vkCmdBindIndexBuffer(secondary, vk_mesh->index_buffer(), 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(secondary, vk_mesh->index_count(), 1, 0, 0, 0);
    } else {
        vkCmdDraw(secondary, vk_mesh->vertex_count(), 1, 0, 0);
    }

    vkEndCommandBuffer(secondary);
    execute_secondary(secondary);

    // 恢复 inline secondary CB 供后续 2D/ImGui/状态命令使用
    begin_inline_secondary();
}

void VulkanBackend::draw_indexed(RHIMeshHandle mesh, RHIShaderHandle shader) {
    draw_mesh(mesh, shader);
}

RHIMeshHandle VulkanBackend::create_mesh() {
    uint32_t index = mesh_pool_.allocate(&device_);
    return {index, mesh_pool_.generation(index)};
}

RHIShaderHandle VulkanBackend::create_shader() {
    uint32_t index = shader_pool_.allocate(&device_, &swapchain_);
    return {index, shader_pool_.generation(index)};
}

RHITextureHandle VulkanBackend::create_texture() {
    uint32_t index = texture_pool_.allocate(&device_);
    return {index, texture_pool_.generation(index)};
}

RHIFramebufferHandle VulkanBackend::create_framebuffer() {
    uint32_t index = framebuffer_pool_.allocate(&device_, &swapchain_);
    return {index, framebuffer_pool_.generation(index)};
}

void VulkanBackend::destroy_mesh(RHIMeshHandle handle) {
    mesh_pool_.deallocate(handle.index);
}

void VulkanBackend::destroy_shader(RHIShaderHandle handle) {
    shader_pool_.deallocate(handle.index);
}

void VulkanBackend::destroy_texture(RHITextureHandle handle) {
    texture_pool_.deallocate(handle.index);
}

void VulkanBackend::destroy_framebuffer(RHIFramebufferHandle handle) {
    framebuffer_pool_.deallocate(handle.index);
}

IMesh* VulkanBackend::mesh(RHIMeshHandle handle) {
    return mesh_pool_.get_if_alive(handle.index, handle.generation);
}

IShader* VulkanBackend::shader(RHIShaderHandle handle) {
    return shader_pool_.get_if_alive(handle.index, handle.generation);
}

ITexture* VulkanBackend::texture(RHITextureHandle handle) {
    return texture_pool_.get_if_alive(handle.index, handle.generation);
}

IFramebuffer* VulkanBackend::framebuffer(RHIFramebufferHandle handle) {
    return framebuffer_pool_.get_if_alive(handle.index, handle.generation);
}

const char* VulkanBackend::api_name() const {
    return "Vulkan";
}

const char* VulkanBackend::api_version() const {
    return "1.2";
}

RenderBackendCapabilities VulkanBackend::get_capabilities() const {
    RenderBackendCapabilities caps;
    caps.supports_vsync_control = false; // 当前通过 present mode 间接控制，未暴露 swap interval
    caps.supports_gpu_busy_spin = false;
    caps.supports_nv_delay_before_swap = false;
    caps.supports_dynamic_state = supports_dynamic_state_;
    caps.max_texture_slots = 32;
    caps.max_push_constant_size = 128; // Vulkan 1.2 最小保证值
    caps.supports_srgb = true;
    caps.supports_depth32f = true;
    caps.supports_r8 = true;
    caps.supports_rgba16f = true;
    return caps;
}

namespace {

void save_bgr_bmp(const std::string& path, const unsigned char* bgr_data,
                  uint32_t width, uint32_t height) {
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        GLOG_ERROR("save_bgr_bmp: failed to open '{}'", path);
        return;
    }

    const uint32_t row_size = (width * 3 + 3) & ~3u;
    const uint32_t data_size = row_size * height;
    const uint32_t file_size = 54 + data_size;

    unsigned char header[54] = {};
    header[0] = 'B'; header[1] = 'M';
    *reinterpret_cast<uint32_t*>(&header[2]) = file_size;
    *reinterpret_cast<uint32_t*>(&header[10]) = 54;
    *reinterpret_cast<uint32_t*>(&header[14]) = 40;
    *reinterpret_cast<uint32_t*>(&header[18]) = width;
    *reinterpret_cast<int32_t*>(&header[22]) = static_cast<int32_t>(height);
    *reinterpret_cast<uint16_t*>(&header[26]) = 1;
    *reinterpret_cast<uint16_t*>(&header[28]) = 24;
    *reinterpret_cast<uint32_t*>(&header[34]) = data_size;

    file.write(reinterpret_cast<char*>(header), 54);

    std::vector<unsigned char> row(row_size, 0);
    for (int y = static_cast<int>(height) - 1; y >= 0; --y) {
        const unsigned char* src = bgr_data + static_cast<std::size_t>(y) * width * 4;
        for (uint32_t x = 0; x < width; ++x) {
            row[x * 3 + 0] = src[x * 4 + 0];
            row[x * 3 + 1] = src[x * 4 + 1];
            row[x * 3 + 2] = src[x * 4 + 2];
        }
        file.write(reinterpret_cast<char*>(row.data()), row_size);
    }
}

} // namespace

void VulkanBackend::request_screenshot(const std::string& path) {
    screenshot_path_ = path;
    screenshot_frame_ = frame_count_ + 1;
}

void VulkanBackend::save_screenshot(const std::string& path) {
    VkDevice dev = device_.device();
    VkQueue queue = device_.graphics_queue();
    uint32_t queue_family = device_.graphics_queue_family();

    VkImage src_image = swapchain_.image(current_image_);
    VkExtent2D extent = swapchain_.extent();
    VkDeviceSize size = static_cast<VkDeviceSize>(extent.width * extent.height * 4);

    VulkanBuffer staging;
    if (!staging.init(&device_, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        GLOG_ERROR("VulkanBackend: failed to create screenshot staging buffer");
        return;
    }

    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = queue_family;
    VkCommandPool pool = VK_NULL_HANDLE;
    vkCreateCommandPool(dev, &pool_info, nullptr, &pool);

    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = pool;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(dev, &alloc_info, &cmd);

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin_info);

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = src_image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {extent.width, extent.height, 1};
    vkCmdCopyImageToBuffer(cmd, src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           staging.buffer(), 1, &region);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = 0;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);

    save_bgr_bmp(path, static_cast<const unsigned char*>(staging.mapped()),
                 extent.width, extent.height);

    vkFreeCommandBuffers(dev, pool, 1, &cmd);
    vkDestroyCommandPool(dev, pool, nullptr);
    staging.shutdown();

    GLOG_INFO("VulkanBackend: screenshot saved to '{}'", path);
}

std::unique_ptr<IRenderer2D> VulkanBackend::create_renderer2d() {
    return std::make_unique<VulkanRenderer2D>();
}

std::unique_ptr<IImGuiBackend> VulkanBackend::create_imgui_backend() {
    return std::make_unique<VulkanImGuiBackend>(this);
}

void VulkanBackend::set_validation_enabled(bool enabled) {
    VulkanInstance::set_enable_validation(enabled);
}

} // namespace gryce_engine::render
