#pragma once

#include <cstdint>
#include <string>

#include "math/math.h"
#include "render/rhi_handle.h"

namespace gryce_engine::render {

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
    Clear,
    ClearDepth,
    BindFramebuffer,
    SetShader,
    SetTexture,
    SetUniformInt,
    SetUniformFloat,
    SetUniformVec3,
    SetUniformVec4,
    SetUniformMat4,
    DrawMesh,
    DrawIndexed,
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
        struct { bool enabled; } cull_face;
        struct { bool enabled; int iterations; } gpu_busy_spin;
        int swap_interval;
        int uniform_int;
        float uniform_float;
    };

    // 句柄与字符串（联合体不能含非平凡析构，所以放外面）
    RHIShaderHandle shader;
    RHITextureHandle texture;
    RHIMeshHandle mesh;
    RHIFramebufferHandle framebuffer;
    std::string uniform_name;
    math::Vector3f uniform_vec3;
    math::Vector4f uniform_vec4;
    math::Matrix4f uniform_mat4;

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

    static RenderCommandTyped make_set_cull_face(bool enabled) {
        RenderCommandTyped cmd;
        cmd.type = RenderCommandType::SetCullFace;
        cmd.cull_face = {enabled};
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

    static RenderCommandTyped make_set_texture(RHIShaderHandle s, RHITextureHandle t, int slot, std::string name) {
        RenderCommandTyped cmd;
        cmd.type = RenderCommandType::SetTexture;
        cmd.shader = s;
        cmd.texture = t;
        // slot 复用 uniform_int
        cmd.uniform_int = slot;
        cmd.uniform_name = std::move(name);
        return cmd;
    }

    static RenderCommandTyped make_set_uniform_int(RHIShaderHandle s, std::string name, int value) {
        RenderCommandTyped cmd;
        cmd.type = RenderCommandType::SetUniformInt;
        cmd.shader = s;
        cmd.uniform_name = std::move(name);
        cmd.uniform_int = value;
        return cmd;
    }

    static RenderCommandTyped make_set_uniform_float(RHIShaderHandle s, std::string name, float value) {
        RenderCommandTyped cmd;
        cmd.type = RenderCommandType::SetUniformFloat;
        cmd.shader = s;
        cmd.uniform_name = std::move(name);
        cmd.uniform_float = value;
        return cmd;
    }

    static RenderCommandTyped make_set_uniform_vec3(RHIShaderHandle s, std::string name, const math::Vector3f& value) {
        RenderCommandTyped cmd;
        cmd.type = RenderCommandType::SetUniformVec3;
        cmd.shader = s;
        cmd.uniform_name = std::move(name);
        cmd.uniform_vec3 = value;
        return cmd;
    }

    static RenderCommandTyped make_set_uniform_vec4(RHIShaderHandle s, std::string name, const math::Vector4f& value) {
        RenderCommandTyped cmd;
        cmd.type = RenderCommandType::SetUniformVec4;
        cmd.shader = s;
        cmd.uniform_name = std::move(name);
        cmd.uniform_vec4 = value;
        return cmd;
    }

    static RenderCommandTyped make_set_uniform_mat4(RHIShaderHandle s, std::string name, const math::Matrix4f& value) {
        RenderCommandTyped cmd;
        cmd.type = RenderCommandType::SetUniformMat4;
        cmd.shader = s;
        cmd.uniform_name = std::move(name);
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
};

} // namespace gryce_engine::render
