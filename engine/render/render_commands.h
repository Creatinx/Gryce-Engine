#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "math/math.h"
#include "render/buffer.h"
#include "render/rhi_handle.h"

namespace gryce_engine::render {

struct PostProcessParams;

// ---------------------------------------------------------------------------
// 混合因子与方程枚举（后端映射到自身 API 常量）
// ---------------------------------------------------------------------------
enum class BlendFactor {
    Zero,
    One,
    SrcAlpha,
    OneMinusSrcAlpha,
    DstAlpha,
    OneMinusDstAlpha,
    SrcColor,
    OneMinusSrcColor,
    DstColor,
    OneMinusDstColor
};

enum class BlendEquation {
    Add,
    Subtract,
    ReverseSubtract,
    Min,
    Max
};

// ---------------------------------------------------------------------------
// CullMode — 剔除面模式
// ---------------------------------------------------------------------------
enum class CullMode : uint8_t {
    None,   // 不剔除
    Back,   // 剔除背面（默认）
    Front   // 剔除正面（用于阴影贴图渲染以减少 Peter Panning）
};

// ---------------------------------------------------------------------------
// RenderCommandType — 结构化渲染命令类型
// ---------------------------------------------------------------------------
enum class RenderCommandType : uint8_t {
    None,
    SetViewport,
    SetScissor,
    SetDepthTest,
    SetDepthWrite,
    SetBlend,
    SetBlendFunc,
    SetBlendEquation,
    SetCullFace,
    SetPolygonMode,
    Clear,
    ClearDepth,
    BindFramebuffer,
    SetShader,
    SetTexture,
    SetUniformInt,
    SetUniformFloat,
    SetUniformVec2,
    SetUniformVec3,
    SetUniformVec4,
    SetUniformMat4,
    DrawMesh,
    DrawIndexed,
    // 存储/计算/间接绘制命令：原实现走 std::function（每条一次可能堆分配），
    // 改为 typed 零分配。数据块/绑定名用 shared_ptr 承载（联合体不能放非平凡类型）。
    DispatchCompute,
    BindStorageBuffer,
    BindInstanceStream,
    DrawIndexedIndirect,
    DrawArraysIndirect,
    UpdateBuffer,
    CopyTextureToBuffer,
    EndRenderPassForTransfer,
    CreateBufferStorage,
    SetPostProcessParams,
    ResolveFramebuffer,
    SetSwapInterval,
    SetGpuBusySpin,
    CustomLambda
};

// ---------------------------------------------------------------------------
// RenderCommandTyped — 类型化渲染命令（SOA-friendly，便于排序/合并）
// ---------------------------------------------------------------------------
struct RenderCommandTyped {
    RenderCommandType type = RenderCommandType::None;

    union {
        struct { float r, g, b, a; } clear_color;
        struct { int x, y, w, h; } viewport;
        struct { int x, y, w, h; } scissor;
        struct { bool enabled; } depth_test;
        struct { bool enabled; } depth_write;
        struct { bool enabled; } blend;
        struct { BlendFactor src; BlendFactor dst; } blend_func;
        BlendEquation blend_equation;
        struct { CullMode mode; } cull_face;
        struct { int mode; } polygon_mode;   // 0 = 填充，1 = 线框
        struct { bool enabled; int iterations; } gpu_busy_spin;
        struct { uint32_t gx, gy, gz; } compute_grid;
        int swap_interval;
        int uniform_int;
        float uniform_float;
    };

    // 句柄与字符串（联合体不能含非平凡析构，所以放外面）
    RHIShaderHandle shader;
    RHITextureHandle texture;
    RHIMeshHandle mesh;
    RHIFramebufferHandle framebuffer;
    // ResolveFramebuffer：解析目标帧缓冲（src 在 framebuffer 成员）
    RHIFramebufferHandle framebuffer_dst;
    // SetTexture：true 时按"原始深度"绑定（OpenGL 绑定关闭比较模式的 sampler，
    // 供 PCSS blocker search / SSAO 重建深度读取真实深度值）
    bool texture_raw_depth = false;
    // uniform 名只保存指针，不做 std::string 拷贝。
    // 所有调用点传的都是编译期字符串字面量（"uModel" / "uView" …），
    // 生命周期覆盖整条命令的执行；原实现按值收 std::string，每次 set_uniform_*
    // 都要构造（可能堆分配）一个字符串 —— 在数千实体的场景里是主要 CPU 开销。
    // 命令在同一个进程内、当帧内即被消费，不存在跨进程/跨帧传递。
    const char* uniform_name = nullptr;
    math::Vector2f uniform_vec2;
    math::Vector3f uniform_vec3;
    math::Vector4f uniform_vec4;
    math::Matrix4f uniform_mat4;
    // SetPostProcessParams：后处理参数块。体积大（60+ 字段）且每效果每帧只发一次，
    // 用 shared_ptr 承载，避免内联进命令项把每条命令都撑大。
    // ★ 为什么必须走命令缓冲：D3D12 的 uniform 影子内存由渲染线程按帧复位，
    //   主线程在录制期直接写 IShader::set_post_process_params 会被复位擦掉
    //   （SSR 偶发整帧丢失的根因，见 d3d12_shader.cpp on_begin_frame 的说明）。
    std::shared_ptr<const PostProcessParams> post_process_params;
    // BindStorageBuffer / BindInstanceStream / DrawIndexedIndirect /
    // DrawArraysIndirect / UpdateBuffer / CopyTextureToBuffer /
    // CreateBufferStorage 共用的 buffer 句柄（args 间接参数块也是 buffer）。
    RHIBufferHandle buffer;
    // UpdateBuffer / CreateBufferStorage 的数据块：shared_ptr 承载避免内联
    // 大块数据撑大命令项；闭包生命周期由命令缓冲管理，与 post_process_params 同模式。
    std::shared_ptr<const std::vector<uint8_t>> buffer_data;
    // BindStorageBuffer 的绑定名：调用方可能传栈上临时量，必须拷贝进命令。
    std::shared_ptr<const std::string> storage_name;
    // UpdateBuffer / CreateBufferStorage 的尺寸与偏移；CreateBufferStorage 的用途。
    size_t buffer_size = 0;
    size_t buffer_offset = 0;
    BufferUsage buffer_usage = BufferUsage::Static;

    RenderCommandTyped() = default;

    static RenderCommandTyped make_clear(float r, float g, float b, float a) {
        RenderCommandTyped cmd;
        cmd.type = RenderCommandType::Clear;
        cmd.clear_color = {r, g, b, a};
        return cmd;
    }

    static RenderCommandTyped make_clear_depth() {
        RenderCommandTyped cmd;
        cmd.type = RenderCommandType::ClearDepth;
        return cmd;
    }

    static RenderCommandTyped make_set_scissor(int x, int y, int w, int h) {
        RenderCommandTyped cmd;
        cmd.type = RenderCommandType::SetScissor;
        cmd.scissor = {x, y, w, h};
        return cmd;
    }

    static RenderCommandTyped make_set_viewport(int x, int y, int w, int h) {
        RenderCommandTyped cmd;
        cmd.type = RenderCommandType::SetViewport;
        cmd.viewport = {x, y, w, h};
        return cmd;
    }

    static RenderCommandTyped make_set_depth_test(bool enabled) {
        RenderCommandTyped cmd;
        cmd.type = RenderCommandType::SetDepthTest;
        cmd.depth_test = {enabled};
        return cmd;
    }

    static RenderCommandTyped make_set_depth_write(bool enabled) {
        RenderCommandTyped cmd;
        cmd.type = RenderCommandType::SetDepthWrite;
        cmd.depth_write = {enabled};
        return cmd;
    }

    static RenderCommandTyped make_set_blend(bool enabled) {
        RenderCommandTyped cmd;
        cmd.type = RenderCommandType::SetBlend;
        cmd.blend = {enabled};
        return cmd;
    }

    static RenderCommandTyped make_set_blend_func(BlendFactor src, BlendFactor dst) {
        RenderCommandTyped cmd;
        cmd.type = RenderCommandType::SetBlendFunc;
        cmd.blend_func = {src, dst};
        return cmd;
    }

    static RenderCommandTyped make_set_blend_equation(BlendEquation mode) {
        RenderCommandTyped cmd;
        cmd.type = RenderCommandType::SetBlendEquation;
        cmd.blend_equation = mode;
        return cmd;
    }

    static RenderCommandTyped make_set_cull_face(CullMode mode) {
        RenderCommandTyped cmd;
        cmd.type = RenderCommandType::SetCullFace;
        cmd.cull_face = {mode};
        return cmd;
    }

    static RenderCommandTyped make_set_polygon_mode(int mode) {
        RenderCommandTyped cmd;
        cmd.type = RenderCommandType::SetPolygonMode;
        cmd.polygon_mode = {mode};
        return cmd;
    }

    static RenderCommandTyped make_bind_framebuffer(RHIFramebufferHandle fb) {
        RenderCommandTyped cmd;
        cmd.type = RenderCommandType::BindFramebuffer;
        cmd.framebuffer = fb;
        return cmd;
    }

    static RenderCommandTyped make_set_shader(RHIShaderHandle s) {
        RenderCommandTyped cmd;
        cmd.type = RenderCommandType::SetShader;
        cmd.shader = s;
        return cmd;
    }

    static RenderCommandTyped make_set_texture(RHIShaderHandle s, RHITextureHandle t, int slot, const char* name) {
        RenderCommandTyped cmd;
        cmd.type = RenderCommandType::SetTexture;
        cmd.shader = s;
        cmd.texture = t;
        // slot 复用 uniform_int
        cmd.uniform_int = slot;
        cmd.uniform_name = name ? name : "";
        return cmd;
    }

    static RenderCommandTyped make_set_texture_raw_depth(RHIShaderHandle s, RHITextureHandle t,
                                                         int slot, const char* name) {
        RenderCommandTyped cmd = make_set_texture(s, t, slot, std::move(name));
        cmd.texture_raw_depth = true;
        return cmd;
    }

    static RenderCommandTyped make_set_uniform_int(RHIShaderHandle s, const char* name, int value) {
        RenderCommandTyped cmd;
        cmd.type = RenderCommandType::SetUniformInt;
        cmd.shader = s;
        cmd.uniform_name = name ? name : "";
        cmd.uniform_int = value;
        return cmd;
    }

    static RenderCommandTyped make_set_uniform_float(RHIShaderHandle s, const char* name, float value) {
        RenderCommandTyped cmd;
        cmd.type = RenderCommandType::SetUniformFloat;
        cmd.shader = s;
        cmd.uniform_name = name ? name : "";
        cmd.uniform_float = value;
        return cmd;
    }

    static RenderCommandTyped make_set_uniform_vec2(RHIShaderHandle s, const char* name, const math::Vector2f& value) {
        RenderCommandTyped cmd;
        cmd.type = RenderCommandType::SetUniformVec2;
        cmd.shader = s;
        cmd.uniform_name = name ? name : "";
        cmd.uniform_vec2 = value;
        return cmd;
    }

    static RenderCommandTyped make_set_uniform_vec3(RHIShaderHandle s, const char* name, const math::Vector3f& value) {
        RenderCommandTyped cmd;
        cmd.type = RenderCommandType::SetUniformVec3;
        cmd.shader = s;
        cmd.uniform_name = name ? name : "";
        cmd.uniform_vec3 = value;
        return cmd;
    }

    static RenderCommandTyped make_set_uniform_vec4(RHIShaderHandle s, const char* name, const math::Vector4f& value) {
        RenderCommandTyped cmd;
        cmd.type = RenderCommandType::SetUniformVec4;
        cmd.shader = s;
        cmd.uniform_name = name ? name : "";
        cmd.uniform_vec4 = value;
        return cmd;
    }

    static RenderCommandTyped make_set_uniform_mat4(RHIShaderHandle s, const char* name, const math::Matrix4f& value) {
        RenderCommandTyped cmd;
        cmd.type = RenderCommandType::SetUniformMat4;
        cmd.shader = s;
        cmd.uniform_name = name ? name : "";
        cmd.uniform_mat4 = value;
        return cmd;
    }

    static RenderCommandTyped make_draw_mesh(RHIMeshHandle m, RHIShaderHandle s) {
        RenderCommandTyped cmd;
        cmd.type = RenderCommandType::DrawMesh;
        cmd.mesh = m;
        cmd.shader = s;
        return cmd;
    }

    static RenderCommandTyped make_draw_indexed(RHIMeshHandle m, RHIShaderHandle s) {
        RenderCommandTyped cmd;
        cmd.type = RenderCommandType::DrawIndexed;
        cmd.mesh = m;
        cmd.shader = s;
        return cmd;
    }

    static RenderCommandTyped make_dispatch_compute(RHIShaderHandle s, uint32_t gx, uint32_t gy,
                                                    uint32_t gz) {
        RenderCommandTyped cmd;
        cmd.type = RenderCommandType::DispatchCompute;
        cmd.shader = s;
        cmd.compute_grid = {gx, gy, gz};
        return cmd;
    }

    static RenderCommandTyped make_bind_storage_buffer(RHIShaderHandle s,
                                                       std::shared_ptr<const std::string> name,
                                                       RHIBufferHandle b) {
        RenderCommandTyped cmd;
        cmd.type = RenderCommandType::BindStorageBuffer;
        cmd.shader = s;
        cmd.storage_name = std::move(name);
        cmd.buffer = b;
        return cmd;
    }

    static RenderCommandTyped make_bind_instance_stream(RHIBufferHandle b) {
        RenderCommandTyped cmd;
        cmd.type = RenderCommandType::BindInstanceStream;
        cmd.buffer = b;
        return cmd;
    }

    static RenderCommandTyped make_draw_indexed_indirect(RHIMeshHandle m, RHIShaderHandle s,
                                                         RHIBufferHandle args, uint32_t offset) {
        RenderCommandTyped cmd;
        cmd.type = RenderCommandType::DrawIndexedIndirect;
        cmd.mesh = m;
        cmd.shader = s;
        cmd.buffer = args;
        cmd.uniform_int = static_cast<int>(offset);
        return cmd;
    }

    static RenderCommandTyped make_draw_arrays_indirect(RHIMeshHandle m, RHIShaderHandle s,
                                                        RHIBufferHandle args, uint32_t offset) {
        RenderCommandTyped cmd;
        cmd.type = RenderCommandType::DrawArraysIndirect;
        cmd.mesh = m;
        cmd.shader = s;
        cmd.buffer = args;
        cmd.uniform_int = static_cast<int>(offset);
        return cmd;
    }

    static RenderCommandTyped make_update_buffer(RHIBufferHandle b,
                                                 std::shared_ptr<const std::vector<uint8_t>> data,
                                                 size_t offset) {
        RenderCommandTyped cmd;
        cmd.type = RenderCommandType::UpdateBuffer;
        cmd.buffer = b;
        cmd.buffer_data = std::move(data);
        cmd.buffer_offset = offset;
        return cmd;
    }

    static RenderCommandTyped make_copy_texture_to_buffer(RHITextureHandle t, RHIBufferHandle b,
                                                          uint32_t offset) {
        RenderCommandTyped cmd;
        cmd.type = RenderCommandType::CopyTextureToBuffer;
        cmd.texture = t;
        cmd.buffer = b;
        cmd.uniform_int = static_cast<int>(offset);
        return cmd;
    }

    static RenderCommandTyped make_end_render_pass_for_transfer() {
        RenderCommandTyped cmd;
        cmd.type = RenderCommandType::EndRenderPassForTransfer;
        return cmd;
    }

    static RenderCommandTyped make_create_buffer_storage(RHIBufferHandle b, size_t size,
                                                         std::shared_ptr<const std::vector<uint8_t>> data,
                                                         BufferUsage usage) {
        RenderCommandTyped cmd;
        cmd.type = RenderCommandType::CreateBufferStorage;
        cmd.buffer = b;
        cmd.buffer_size = size;
        cmd.buffer_data = std::move(data);
        cmd.buffer_usage = usage;
        return cmd;
    }

    static RenderCommandTyped make_set_swap_interval(int interval) {
        RenderCommandTyped cmd;
        cmd.type = RenderCommandType::SetSwapInterval;
        cmd.swap_interval = interval;
        return cmd;
    }

    static RenderCommandTyped make_set_gpu_busy_spin(bool enabled, int iterations) {
        RenderCommandTyped cmd;
        cmd.type = RenderCommandType::SetGpuBusySpin;
        cmd.gpu_busy_spin = {enabled, iterations};
        return cmd;
    }

    static RenderCommandTyped make_set_post_process_params(
        RHIShaderHandle s, std::shared_ptr<const PostProcessParams> params) {
        RenderCommandTyped cmd;
        cmd.type = RenderCommandType::SetPostProcessParams;
        cmd.shader = s;
        cmd.post_process_params = std::move(params);
        return cmd;
    }

    static RenderCommandTyped make_resolve_framebuffer(RHIFramebufferHandle src,
                                                       RHIFramebufferHandle dst) {
        RenderCommandTyped cmd;
        cmd.type = RenderCommandType::ResolveFramebuffer;
        cmd.framebuffer = src;
        cmd.framebuffer_dst = dst;
        return cmd;
    }
};

} // namespace gryce_engine::render
