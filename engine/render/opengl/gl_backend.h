#pragma once

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <array>
#include <string>
#include <vector>

#include "render/render.h"
#include "render/gpu_profiler.h"
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
    // 引擎槽位 → GL 纹理单元（见 IRenderBackend::texture_unit_for_slot 说明）
    int texture_unit_for_slot(int slot) const override;
    void set_viewport(int x, int y, int w, int h) override;
    void set_viewport(int x, int y, int w, int h, uint32_t viewport_index) override;
    void set_scissor(int x, int y, int w, int h) override;
    void set_scissor(int x, int y, int w, int h, uint32_t viewport_index) override;
    void set_depth_test(bool enabled) override;
    void set_depth_write(bool enabled) override;
    void set_blend(bool enabled) override;
    void set_blend_func(BlendFactor src_factor, BlendFactor dst_factor) override;
    void set_blend_equation(BlendEquation mode) override;
    void set_cull_face(CullMode mode) override;
    void bind_framebuffer(RHIFramebufferHandle fb) override;
    void unbind_framebuffer() override;

    void draw_mesh(RHIMeshHandle mesh, RHIShaderHandle shader) override;
    void draw_indexed(RHIMeshHandle mesh, RHIShaderHandle shader) override;

    RHIMeshHandle create_mesh() override;
    RHIShaderHandle create_shader() override;
    RHITextureHandle create_texture() override;
    RHIFramebufferHandle create_framebuffer() override;
    RHIBufferHandle create_buffer() override;

    void destroy_mesh(RHIMeshHandle handle) override;
    void destroy_shader(RHIShaderHandle handle) override;
    void destroy_texture(RHITextureHandle handle) override;
    void destroy_framebuffer(RHIFramebufferHandle handle) override;
    void destroy_buffer(RHIBufferHandle handle) override;

    IMesh* mesh(RHIMeshHandle handle) override;
    IShader* shader(RHIShaderHandle handle) override;
    ITexture* texture(RHITextureHandle handle) override;
    IFramebuffer* framebuffer(RHIFramebufferHandle handle) override;
    IBuffer* buffer(RHIBufferHandle handle) override;

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
    void capture_frame_to_file(const std::string& path) override;
    bool capture_frame_rgba(std::vector<uint8_t>& out, int& w, int& h) override;
    std::unique_ptr<IRenderer2D> create_renderer2d() override;
    std::unique_ptr<IImGuiBackend> create_imgui_backend() override;
    void set_validation_enabled(bool enabled) override;
    // ---- GPU 分段计时（GL_TIME_ELAPSED query，异步读回，不等 GPU）----
    bool gpu_profile_supported() const override;
    void set_gpu_profiling(bool enabled) override;
    void gpu_profile_begin(const char* name) override;
    void gpu_profile_end() override;
    void gpu_profile_frame_boundary() override;
    void gpu_profile_dump() override;
    void gpu_profile_reset() override;

 private:
    GLFWwindow* window_ = nullptr;
    GLFramePacing frame_pacing_;
    std::string screenshot_path_;
    int screenshot_frame_ = -1;
    int frame_count_ = 0;
    // GL_MAX_TEXTURE_IMAGE_UNITS（片段阶段可用纹理单元数，Intel 集显为 32）
    int max_texture_units_ = 32;

    RHIResourcePool<GLMesh> mesh_pool_;
    RHIResourcePool<GLShader> shader_pool_;
    RHIResourcePool<GLTexture> texture_pool_;
    RHIResourcePool<GLFramebuffer> framebuffer_pool_;
    RHIResourcePool<GLStorageBuffer> buffer_pool_;

    // 状态缓存：避免向 driver 下发重复的状态切换命令。
    bool state_cache_valid_ = false;
    bool depth_test_enabled_ = false;
    bool depth_write_enabled_ = true;
    bool blend_enabled_ = false;
    CullMode cull_face_mode_ = CullMode::Back;
    BlendFactor blend_src_ = BlendFactor::One;
    BlendFactor blend_dst_ = BlendFactor::Zero;
    BlendEquation blend_equation_ = BlendEquation::Add;
    int viewport_x_ = 0, viewport_y_ = 0, viewport_w_ = 0, viewport_h_ = 0;
    int scissor_x_ = 0, scissor_y_ = 0, scissor_w_ = 0, scissor_h_ = 0;

    void save_screenshot(const std::string& path);

    // GPU 计时：每帧一个槽（环形），每槽预留 k_gpu_max_scopes 对 query。
    // 结果在槽被复用时才读（且先查 GL_QUERY_RESULT_AVAILABLE），所以不会
    // 让 CPU 等 GPU。
    static constexpr int k_gpu_slots = 3;
    static constexpr int k_gpu_max_scopes = 24;
    struct GpuScopeQuery {
        // GL_TIME_ELAPSED：glBeginQuery 绑定 query，glEndQuery 结束后结果就写在
        // **这个**对象上，所以一段只需要一个 query。
        unsigned int id = 0;
        std::string name;
    };
    std::array<std::vector<GpuScopeQuery>, k_gpu_slots> gpu_queries_;
    std::array<int, k_gpu_slots> gpu_scope_count_{};
    int gpu_slot_ = 0;
    int gpu_pending_scope_ = -1;
    bool gpu_profiling_enabled_ = false;
    bool gpu_profiling_ready_ = false;
    bool gpu_nested_warned_ = false;
    GpuProfilerStats gpu_stats_;
};

} // namespace gryce_engine::render
