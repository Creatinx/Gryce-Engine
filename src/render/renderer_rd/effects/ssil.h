#pragma once

#include "render/rhi_handle.h"
#include "render/texture.h"
#include "render/framebuffer.h"
#include "render/shader.h"

namespace gryce_engine::render {

class RenderContext;

// ---------------------------------------------------------------------------
// SSIL_RD — 屏幕空间间接光照
// 参考 Godot 的 SSIL 实现，使用重要性采样 + 多方向从深度/法线重建间接光照。
// ---------------------------------------------------------------------------
class SSIL_RD {
public:
    SSIL_RD() = default;
    ~SSIL_RD() { destroy(); }

    bool init(RenderContext* ctx);
    void destroy();

    bool create_targets(int width, int height);
    void destroy_targets();

    void render(RenderContext* ctx,
                RHITextureHandle color_tex,
                RHITextureHandle depth_tex,
                RHITextureHandle normal_roughness_tex,
                const PostProcessParams& params,
                int viewport_w, int viewport_h);

    RHITextureHandle ssil_tex() const { return ssil_tex_[ping_]; }
    bool valid() const { return initialized_; }

private:
    RenderContext* ctx_ = nullptr;

    // 双缓冲 SSIL 输出（半分辨率）
    RHITextureHandle ssil_tex_[2];
    RHIFramebufferHandle ssil_fbo_[2];
    int ssil_w_ = 0;
    int ssil_h_ = 0;
    int ping_ = 0;

    // Shader 句柄
    RHIShaderHandle ssil_shader_;
    RHIShaderHandle ssil_blur_shader_;
    RHIMeshHandle fullscreen_mesh_;

    bool initialized_ = false;
};

} // namespace gryce_engine::render