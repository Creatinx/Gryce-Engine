#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "render/render2d.h"
#include "render/imgui_backend.h"
#include "render/rhi_handle.h"
#include "render/render_commands.h"

namespace gryce_engine::render {

// 前向声明
class IMesh;
class IShader;
class ITexture;
class IFramebuffer;

// ---------------------------------------------------------------------------
// RenderBackendCapabilities — 后端能力查询
// 将 vendor-specific 特性、纹理格式支持、动态状态等显式化，避免 base 接口泄漏。
// ---------------------------------------------------------------------------
struct RenderBackendCapabilities {
    // 通用能力
    bool supports_compute = false;
    bool supports_geometry_shader = false;
    bool supports_dynamic_state = false;        // Vulkan extended dynamic state 等
    uint32_t max_texture_slots = 16;
    uint32_t max_push_constant_size = 128;

    // 帧率/呈现相关
    bool supports_vsync_control = false;        // swap interval / present mode
    bool supports_gpu_busy_spin = false;        // 主动占用 GPU 的 pacing 技巧
    bool supports_nv_delay_before_swap = false; // NVIDIA WGL_NV_delay_before_swap

    // 纹理格式
    bool supports_srgb = true;
    bool supports_depth32f = true;
    bool supports_r8 = true;
    bool supports_rgba16f = false;
};

// ---------------------------------------------------------------------------
// IRenderBackend — 渲染后端抽象接口
// ---------------------------------------------------------------------------
class IRenderBackend {
public:
    virtual ~IRenderBackend() = default;

    // 生命周期
    virtual bool init(void* native_window) = 0;
    virtual void shutdown() = 0;

    // 上下文管理（把 GLFW/GL 的 make_current / swap_buffers 抽象出来，
    // 便于 Vulkan backend 实现空操作或 surface 相关逻辑）
    virtual void make_current(void* native_window) = 0;
    virtual void release_context() = 0;

    // 帧循环
    virtual void begin_frame() = 0;
    virtual void end_frame() = 0;

    // 将已提交命令 flush 到驱动，不等待 GPU 完成（用于减少退出延迟）
    virtual void flush_gpu() {}

    // 强制 GPU 完成所有已提交命令（真正 idle，用于销毁资源/后端前）
    virtual void wait_gpu_idle() {}

    // 兼容旧名：默认委托给 wait_gpu_idle()
    virtual void finish_gpu() { wait_gpu_idle(); }

    // 状态
    virtual void clear(float r, float g, float b, float a) = 0;
    virtual void clear_depth() {}
    virtual void set_viewport(int x, int y, int w, int h) = 0;
    virtual void set_viewport(int x, int y, int w, int h, uint32_t viewport_index) {
        (void)x; (void)y; (void)w; (void)h; (void)viewport_index;
    }
    virtual void set_scissor(int x, int y, int w, int h) {
        (void)x; (void)y; (void)w; (void)h;
    }
    virtual void set_scissor(int x, int y, int w, int h, uint32_t viewport_index) {
        (void)x; (void)y; (void)w; (void)h; (void)viewport_index;
    }
    virtual uint32_t max_viewports() const { return 1; }
    virtual void set_depth_test(bool enabled) = 0;
    // 深度写入开关（透明物体：测试开、写入关）。默认实现为空。
    virtual void set_depth_write(bool enabled) { (void)enabled; }
    virtual void set_blend(bool enabled) = 0;
    virtual void set_blend_func(BlendFactor src_factor, BlendFactor dst_factor) {
        (void)src_factor; (void)dst_factor;
    }
    virtual void set_blend_equation(BlendEquation mode) { (void)mode; }
    virtual void set_cull_face(bool enabled) = 0;

    // 绘制（使用 RHI 句柄）
    virtual void draw_mesh(RHIMeshHandle mesh, RHIShaderHandle shader) = 0;
    virtual void draw_indexed(RHIMeshHandle mesh, RHIShaderHandle shader) = 0;

    // 工厂方法：创建后端特定资源，返回句柄
    virtual RHIMeshHandle create_mesh() = 0;
    virtual RHIShaderHandle create_shader() = 0;
    virtual RHITextureHandle create_texture() = 0;
    virtual RHIFramebufferHandle create_framebuffer() = 0;

    // 销毁资源（渲染线程安全）
    virtual void destroy_mesh(RHIMeshHandle handle) = 0;
    virtual void destroy_shader(RHIShaderHandle handle) = 0;
    virtual void destroy_texture(RHITextureHandle handle) = 0;
    virtual void destroy_framebuffer(RHIFramebufferHandle handle) = 0;

    // 通过句柄访问实际对象（供渲染线程使用）
    virtual IMesh* mesh(RHIMeshHandle handle) = 0;
    virtual IShader* shader(RHIShaderHandle handle) = 0;
    virtual ITexture* texture(RHITextureHandle handle) = 0;
    virtual IFramebuffer* framebuffer(RHIFramebufferHandle handle) = 0;

    // 帧缓冲
    virtual void bind_framebuffer(RHIFramebufferHandle fb) { (void)fb; }
    virtual void unbind_framebuffer() {}

    // 帧率 / 呈现控制
    virtual void set_swap_interval(int interval) { (void)interval; }
    virtual void set_gpu_busy_spin(bool enabled, int iterations) {
        (void)enabled; (void)iterations;
    }
    virtual void set_nv_delay_before_swap(float seconds) { (void)seconds; }
    virtual bool supports_nv_delay_before_swap() const { return false; }

    // 信息
    virtual const char* api_name() const = 0;
    virtual const char* api_version() const = 0;

    // 能力查询（将 vendor-specific 特性、格式支持等显式化）
    virtual RenderBackendCapabilities get_capabilities() const = 0;

    // Screenshot: backend captures current swapchain/frontbuffer to a BMP file.
    virtual void request_screenshot(const std::string& path) { (void)path; }

    // Factory for 2D renderer (OpenGL -> Renderer2D, Vulkan -> VulkanRenderer2D).
    virtual std::unique_ptr<IRenderer2D> create_renderer2d() { return nullptr; }

    // Factory for ImGui render backend.
    virtual std::unique_ptr<IImGuiBackend> create_imgui_backend() { return nullptr; }

    // Enable backend validation (Vulkan validation layers). Must be called before init().
    virtual void set_validation_enabled(bool enabled) { (void)enabled; }
};

enum class RenderAPI {
    OpenGL,
    Vulkan
};

// 根据 API 创建对应后端（OpenGL / Vulkan）
std::unique_ptr<IRenderBackend> create_render_backend(RenderAPI api);

} // namespace gryce_engine::render
