#pragma once

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <string>

#include "render/render.h"
#include "render/rhi_resource_pool.h"
#include "gl_frame_pacing.h"
#include "gl_buffer.h"
#include "gl_shader.h"
#include "gl_texture.h"
#include "gl_framebuffer.h"

namespace gryce_engine::render {

class GLMesh;
class GLShader;
class GLTexture;
class GLFramebuffer;

// ---------------------------------------------------------------------------
// GLBackend — OpenGL 渲染后端
// ---------------------------------------------------------------------------
class GLBackend : public IRenderBackend {
public:
    GLBackend();
    ~GLBackend() override;

    bool init(void* native_window) override;
    void shutdown() override;

    void make_current(void* native_window) override;
    void release_context() override;

    void begin_frame() override;
    void end_frame() override;
    void flush_gpu() override;
    void wait_gpu_idle() override;

    void clear(float r, float g, float b, float a) override;
    void clear_depth() override;
    void set_viewport(int x, int y, int w, int h) override;
    void set_viewport(int x, int y, int w, int h, uint32_t viewport_index) override;
    void set_scissor(int x, int y, int w, int h) override;
    void set_scissor(int x, int y, int w, int h, uint32_t viewport_index) override;
    void set_depth_test(bool enabled) override;
    void set_depth_write(bool enabled) override;
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

    // 帧率 / 呈现控制
    void set_swap_interval(int interval) override;
    void set_gpu_busy_spin(bool enabled, int iterations) override;
    void set_nv_delay_before_swap(float seconds) override;
    bool supports_nv_delay_before_swap() const override;

    const char* api_name() const override;
    const char* api_version() const override;
    RenderBackendCapabilities get_capabilities() const override;

    // 原生窗口句柄（用于渲染线程绑定 GL context）
    GLFWwindow* native_handle() const { return window_; }

    void request_screenshot(const std::string& path) override;
    std::unique_ptr<IRenderer2D> create_renderer2d() override;
    std::unique_ptr<IImGuiBackend> create_imgui_backend() override;
    void set_validation_enabled(bool enabled) override;

private:
    GLFWwindow* window_ = nullptr;
    GLFramePacing frame_pacing_;
    std::string screenshot_path_;
    int screenshot_frame_ = -1;
    int frame_count_ = 0;

    RHIResourcePool<GLMesh> mesh_pool_;
    RHIResourcePool<GLShader> shader_pool_;
    RHIResourcePool<GLTexture> texture_pool_;
    RHIResourcePool<GLFramebuffer> framebuffer_pool_;

    // 状态缓存：避免向 driver 下发重复的状态切换命令。
    bool state_cache_valid_ = false;
    bool depth_test_enabled_ = false;
    bool depth_write_enabled_ = true;
    bool blend_enabled_ = false;
    bool cull_face_enabled_ = false;
    BlendFactor blend_src_ = BlendFactor::One;
    BlendFactor blend_dst_ = BlendFactor::Zero;
    BlendEquation blend_equation_ = BlendEquation::Add;
    int viewport_x_ = 0, viewport_y_ = 0, viewport_w_ = 0, viewport_h_ = 0;
    int scissor_x_ = 0, scissor_y_ = 0, scissor_w_ = 0, scissor_h_ = 0;

    void save_screenshot(const std::string& path);
};

} // namespace gryce_engine::render