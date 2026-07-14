#pragma once

#include <array>
#include <vulkan/vulkan.h>

#include "render/render.h"
#include "render/rhi_resource_pool.h"
#include "vk_instance.h"
#include "vk_device.h"
#include "vk_swapchain.h"
#include "vk_mesh.h"
#include "vk_shader.h"
#include "vk_texture.h"
#include "vk_framebuffer.h"

struct GLFWwindow;

namespace gryce_engine::render {

class VulkanFramebuffer;

// ---------------------------------------------------------------------------
// VulkanBackend — Vulkan 渲染后端实现
// 支持 multi-viewport、secondary command buffer、动态状态缓存。
// ---------------------------------------------------------------------------
class VulkanBackend : public IRenderBackend {
public:
    VulkanBackend();
    ~VulkanBackend() override;

    bool init(void* native_window) override;
    void shutdown() override;

    void make_current(void* native_window) override;
    void release_context() override;

    void begin_frame() override;
    void end_frame() override;
    void flush_gpu() override;
    void wait_gpu_idle() override;

    void clear(float r, float g, float b, float a) override;
    void clear_depth() override {}
    uint32_t max_viewports() const override;
    void set_viewport(int x, int y, int w, int h) override;
    void set_scissor(int x, int y, int w, int h) override;
    void set_viewport(int x, int y, int w, int h, uint32_t viewport_index) override;
    void set_scissor(int x, int y, int w, int h, uint32_t viewport_index) override;
    void set_depth_test(bool enabled) override;
    void set_blend(bool enabled) override;
    void set_blend_func(BlendFactor src_factor, BlendFactor dst_factor) override;
    void set_blend_equation(BlendEquation mode) override;
    void set_cull_face(bool enabled) override;
    void bind_framebuffer(RHIFramebufferHandle fb) override;
    void unbind_framebuffer() override;

    void draw_mesh(RHIMeshHandle mesh, RHIShaderHandle shader) override;
    void draw_indexed(RHIMeshHandle mesh, RHIShaderHandle shader) override;

    RHIMeshHandle create_mesh() override;
    RHIShaderHandle create_shader() override;
    RHITextureHandle create_texture() override;
    RHIFramebufferHandle create_framebuffer() override;

    void destroy_mesh(RHIMeshHandle handle) override;
    void destroy_shader(RHIShaderHandle handle) override;
    void destroy_texture(RHITextureHandle handle) override;
    void destroy_framebuffer(RHIFramebufferHandle handle) override;

    IMesh* mesh(RHIMeshHandle handle) override;
    IShader* shader(RHIShaderHandle handle) override;
    ITexture* texture(RHITextureHandle handle) override;
    IFramebuffer* framebuffer(RHIFramebufferHandle handle) override;

    void set_swap_interval(int interval) override;
    void set_gpu_busy_spin(bool enabled, int iterations) override {}
    void set_nv_delay_before_swap(float seconds) override {}
    bool supports_nv_delay_before_swap() const override { return false; }

    const char* api_name() const override;
    const char* api_version() const override;
    RenderBackendCapabilities get_capabilities() const override;

    void request_screenshot(const std::string& path) override;
    std::unique_ptr<IRenderer2D> create_renderer2d() override;
    std::unique_ptr<IImGuiBackend> create_imgui_backend() override;
    void set_validation_enabled(bool enabled) override;

    VulkanDevice* device() { return &device_; }
    VulkanSwapchain* swapchain() { return &swapchain_; }
    VkInstance instance() const { return instance_.handle(); }
    VkCommandBuffer current_command_buffer();
    VkCommandBuffer primary_command_buffer() const;

    bool supports_dynamic_state() const { return supports_dynamic_state_; }

    // 2D 渲染前重置可能被 3D 管线改掉的动态状态
    void set_dynamic_state_2d(VkCommandBuffer cmd);

    // 命令缓冲状态缓存辅助函数，避免重复绑定/设置相同状态
    void reset_state_cache();
    void bind_pipeline(VkCommandBuffer cmd, VkPipeline pipeline);
    void bind_descriptor_set(VkCommandBuffer cmd, VkPipelineLayout layout, VkDescriptorSet set);
    void set_viewport_cached(VkCommandBuffer cmd, const VkViewport& viewport, uint32_t index = 0);
    void set_scissor_cached(VkCommandBuffer cmd, const VkRect2D& scissor, uint32_t index = 0);
    void set_cull_mode_cached(VkCommandBuffer cmd, VkCullModeFlags mode);
    void set_front_face_cached(VkCommandBuffer cmd, VkFrontFace face);
    void set_depth_test_cached(VkCommandBuffer cmd, VkBool32 enable);
    void set_depth_write_cached(VkCommandBuffer cmd, VkBool32 enable);

private:
    bool load_dynamic_state_functions();
    void apply_dynamic_state(VkCommandBuffer cmd);

    GLFWwindow* window_ = nullptr;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VulkanInstance instance_;
    VulkanDevice device_;
    VulkanSwapchain swapchain_;

    float clear_r_ = 0.0f;
    float clear_g_ = 0.0f;
    float clear_b_ = 0.0f;
    float clear_a_ = 1.0f;
    uint32_t current_image_ = 0;
    bool initialized_ = false;
    bool in_forward_pass_ = false;
    bool frame_aborted_ = false;
    RHIFramebufferHandle current_framebuffer_;

    RHIResourcePool<VulkanMesh> mesh_pool_;
    RHIResourcePool<VulkanShader> shader_pool_;
    RHIResourcePool<VulkanTexture> texture_pool_;
    RHIResourcePool<VulkanFramebuffer> framebuffer_pool_;

    bool supports_dynamic_state_ = false;
    PFN_vkCmdSetCullModeEXT vk_cmd_set_cull_mode_ = nullptr;
    PFN_vkCmdSetFrontFaceEXT vk_cmd_set_front_face_ = nullptr;
    PFN_vkCmdSetDepthTestEnableEXT vk_cmd_set_depth_test_enable_ = nullptr;
    PFN_vkCmdSetDepthWriteEnableEXT vk_cmd_set_depth_write_enable_ = nullptr;

    // Secondary command buffer management
    VkCommandBuffer inline_secondary_cb_ = VK_NULL_HANDLE;
    bool inline_secondary_recording_ = false;
    VkRenderPass current_render_pass_ = VK_NULL_HANDLE;
    VkFramebuffer current_framebuffer_vk_ = VK_NULL_HANDLE;
    bool render_pass_contents_secondary_ = false;

    void begin_inline_secondary();
    void end_inline_secondary();
    void execute_inline_secondary();
    void end_and_execute_inline_secondary();
    VkCommandBuffer allocate_secondary_cb();
    void free_secondary_cb(VkCommandBuffer cb);
    void execute_secondary(VkCommandBuffer secondary);
    void reset_inline_secondary_state();
    void begin_render_pass_secondary(VkRenderPass rp, VkFramebuffer fb,
                                     const VkClearValue* clears, uint32_t clear_count,
                                     const VkExtent2D& extent);
    void end_current_render_pass();

    // 当前渲染状态缓存（高层 API 状态）
    bool depth_test_enabled_ = true;
    bool blend_enabled_ = false;
    bool cull_face_enabled_ = true;
    BlendFactor blend_src_factor_ = BlendFactor::SrcAlpha;
    BlendFactor blend_dst_factor_ = BlendFactor::OneMinusSrcAlpha;
    BlendEquation blend_equation_ = BlendEquation::Add;

    // 每帧命令缓冲状态缓存，避免冗余 Vulkan 命令
    struct StateCache {
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
        VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
        VkCullModeFlags cull_mode = VK_CULL_MODE_FLAG_BITS_MAX_ENUM;
        VkFrontFace front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        VkBool32 depth_test = VK_FALSE;
        VkBool32 depth_write = VK_FALSE;
    } state_cache_;

    // Multi-viewport 支持
    static constexpr uint32_t k_max_viewports = 8;
    std::array<VkViewport, k_max_viewports> viewports_{};
    std::array<VkRect2D, k_max_viewports> scissors_{};
    uint32_t viewport_count_ = 0;
    uint32_t scissor_count_ = 0;
    uint32_t applied_viewport_count_ = 0;
    uint32_t applied_scissor_count_ = 0;
    std::array<VkViewport, k_max_viewports> applied_viewports_{};
    std::array<VkRect2D, k_max_viewports> applied_scissors_{};

    // 截图请求
    std::string screenshot_path_;
    int screenshot_frame_ = -1;
    int frame_count_ = 0;

    // 每帧临时分配的 secondary command buffers（draw_mesh 专用），在 end_frame 后统一重置
    std::vector<VkCommandBuffer> per_frame_secondary_cbs_;

    void save_screenshot(const std::string& path);
};

} // namespace gryce_engine::render
