#pragma once

#include <cstdint>
#include <string>

#include "render/rhi_handle.h"
#include "render/shader.h"
#include "math/math.h"

namespace gryce_engine::render {

class RenderContext;

// ---------------------------------------------------------------------------
// SubsurfaceScattering_RD — 屏幕空间次表面散射
// 使用可分离的深度加权模糊模拟皮肤/半透明材质的次表面散射效果。
// 参考 Godot 的 ScreenSpaceSubsurfaceScattering 实现。
// ---------------------------------------------------------------------------
class SubsurfaceScattering_RD {
public:
    SubsurfaceScattering_RD() = default;
    ~SubsurfaceScattering_RD() { destroy(); }

    bool init(RenderContext* ctx, const std::string& shader_dir = "res:/shaders");
    void destroy();

    bool create_targets(int width, int height);
    void destroy_targets();

    // 渲染 SSS（水平+垂直模糊+合成到输出 FBO）
    void render(RenderContext* ctx,
                RHITextureHandle scene_color_tex,
                RHITextureHandle depth_tex,
                RHIFramebufferHandle output_fbo,
                const PostProcessParams& params,
                int width, int height);

    bool valid() const { return sss_tex_[0].is_valid(); }

private:
    RenderContext* ctx_ = nullptr;

    // 临时渲染目标（双缓冲：水平模糊后 ping-pong）
    RHITextureHandle sss_tex_[2];
    RHIFramebufferHandle sss_fbo_[2];
    int sss_w_ = 0;
    int sss_h_ = 0;

    // Shader
    RHIShaderHandle sss_horizontal_shader_;
    RHIShaderHandle sss_vertical_shader_;
    RHIShaderHandle sss_composite_shader_;
    RHIMeshHandle fullscreen_mesh_;

    bool initialized_ = false;
    bool targets_valid_ = false;
};

} // namespace gryce_engine::render