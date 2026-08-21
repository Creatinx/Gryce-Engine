#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "render/export.h"
#include "render/rhi_handle.h"
#include "render/render_commands.h"

namespace gryce_engine::render {

class IRenderBackend;

// ---------------------------------------------------------------------------
// RenderingDevice — GPU 后端抽象
// 包装 IRenderBackend，提供更统一的设备级接口。
// 类似于 Godot 的 RenderingDevice，所有 GPU 操作通过此接口完成。
// ---------------------------------------------------------------------------
class GRYCE_RENDERER_API RenderingDevice {
public:
    enum class Backend { Vulkan, OpenGL, D3D12, Metal };

    virtual ~RenderingDevice() = default;

    // 生命周期
    virtual bool init(void* native_window) = 0;
    virtual void shutdown() = 0;
    virtual Backend backend() const = 0;
    virtual const char* api_name() const = 0;
    virtual const char* api_version() const = 0;

    // 上下文管理
    virtual void make_current(void* native_window) = 0;
    virtual void release_context() = 0;

    // 帧循环
    virtual void begin_frame() = 0;
    virtual void end_frame() = 0;
    virtual void present() = 0;
    virtual void wait_gpu_idle() = 0;
    virtual void flush_gpu() = 0;

    // 状态设置
    virtual void set_viewport(int x, int y, int w, int h) = 0;
    virtual void set_scissor(int x, int y, int w, int h) = 0;
    virtual void set_depth_test(bool enabled) = 0;
    virtual void set_depth_write(bool enabled) = 0;
    virtual void set_blend(bool enabled) = 0;
    virtual void set_cull_face(CullMode mode) = 0;
    virtual void clear(float r, float g, float b, float a) = 0;
    virtual void clear_depth() = 0;

    // 资源创建/销毁
    virtual RHIMeshHandle create_mesh() = 0;
    virtual RHIShaderHandle create_shader() = 0;
    virtual RHITextureHandle create_texture() = 0;
    virtual RHIFramebufferHandle create_framebuffer() = 0;
    virtual void destroy_mesh(RHIMeshHandle handle) = 0;
    virtual void destroy_shader(RHIShaderHandle handle) = 0;
    virtual void destroy_texture(RHITextureHandle handle) = 0;
    virtual void destroy_framebuffer(RHIFramebufferHandle handle) = 0;

    // 资源访问
    virtual class IMesh* mesh(RHIMeshHandle handle) = 0;
    virtual class IShader* shader(RHIShaderHandle handle) = 0;
    virtual class ITexture* texture(RHITextureHandle handle) = 0;
    virtual class IFramebuffer* framebuffer(RHIFramebufferHandle handle) = 0;

    // 绘制
    virtual void draw_mesh(RHIMeshHandle mesh, RHIShaderHandle shader) = 0;
    virtual void draw_indexed(RHIMeshHandle mesh, RHIShaderHandle shader) = 0;
    virtual void bind_framebuffer(RHIFramebufferHandle fb) = 0;
    virtual void unbind_framebuffer() = 0;

    // 帧率控制
    virtual void set_swap_interval(int interval) = 0;

    // 能力查询
    virtual struct RenderBackendCapabilities get_capabilities() const = 0;

    // 获取底层 IRenderBackend 指针（兼容现有代码）
    virtual IRenderBackend* native_backend() = 0;

    // 工厂方法
    static std::unique_ptr<RenderingDevice> create(Backend backend);
};

} // namespace gryce_engine::render