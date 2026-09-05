#pragma once

#include "render/rhi_handle.h"
#include "render/texture.h"
#include "render/framebuffer.h"
#include "render/shader.h"

namespace gryce_engine::render {

class RenderContext;
class ITexture;
class IFramebuffer;

// ---------------------------------------------------------------------------
// SSR_RD — 屏幕空间反射
// 使用 Hierarchical Z-Buffer 加速的屏幕空间光线步进。
// 参考 Godot 的 SSR 实现。
// ---------------------------------------------------------------------------
class SSR_RD {
public:
    static constexpr int k_ssr_mip_count = 4;

    SSR_RD() = default;
    ~SSR_RD() { destroy(); }

    bool init(RenderContext* ctx);
    void destroy();

    // 创建/销毁 HiZ 缓冲
    bool create_hiz(int width, int height);
    void destroy_hiz();

    // 渲染 SSR
    void render(RenderContext* ctx,
                RHITextureHandle color_tex,
                RHITextureHandle depth_tex,
                RHITextureHandle normal_roughness_tex,
                const PostProcessParams& params,
                int viewport_w, int viewport_h);

    // 访问
    RHITextureHandle ssr_tex() const { return ssr_tex_; }
    RHIFramebufferHandle ssr_fbo() const { return ssr_fbo_; }
    bool valid() const { return ssr_tex_.is_valid(); }

private:
    RenderContext* ctx_ = nullptr;

    // SSR 输出
    RHITextureHandle ssr_tex_;
    RHIFramebufferHandle ssr_fbo_;

    // HiZ 缓冲
    RHITextureHandle hiz_tex_[k_ssr_mip_count];
    int hiz_w_ = 0;
    int hiz_h_ = 0;

    // Shader 句柄
    RHIShaderHandle ssr_shader_;
    RHIShaderHandle hiz_build_shader_;
    RHIShaderHandle ssr_blur_shader_;
    RHIMeshHandle fullscreen_mesh_;

    bool initialized_ = false;
};

} // namespace gryce_engine::render