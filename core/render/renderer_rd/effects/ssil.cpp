#include "render/renderer_rd/effects/ssil.h"
#include "render/render_context.h"
#include "render/mesh.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::render {

bool SSIL_RD::init(RenderContext* ctx) {
    if (initialized_) return true;
    ctx_ = ctx;
    initialized_ = true;
    return true;
}

void SSIL_RD::destroy() {
    if (!ctx_) return;
    destroy_targets();
    initialized_ = false;
}

bool SSIL_RD::create_targets(int width, int height) {
    destroy_targets();

    ssil_w_ = std::max(16, width / 2);
    ssil_h_ = std::max(16, height / 2);

    for (int i = 0; i < 2; ++i) {
        ssil_tex_[i] = ctx_->create_texture();
        ITexture* tex = ctx_->texture(ssil_tex_[i]);
        if (!ssil_tex_[i].is_valid() || !tex ||
            !tex->create(TextureFormat::RGBA16F, ssil_w_, ssil_h_, nullptr)) {
            return false;
        }
        tex->set_filter(TextureFilter::Linear, TextureFilter::Linear);
        tex->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

        ssil_fbo_[i] = ctx_->create_framebuffer();
        IFramebuffer* fbo = ctx_->framebuffer(ssil_fbo_[i]);
        if (!ssil_fbo_[i].is_valid() || !fbo || !fbo->create(ssil_w_, ssil_h_)) {
            return false;
        }
        fbo->attach_color_texture(tex);
        if (!fbo->is_complete()) return false;
    }

    return true;
}

void SSIL_RD::destroy_targets() {
    if (!ctx_) return;
    for (auto& fbo : ssil_fbo_) {
        if (fbo.is_valid()) { ctx_->destroy_framebuffer(fbo); fbo = {}; }
    }
    for (auto& tex : ssil_tex_) {
        if (tex.is_valid()) { ctx_->destroy_texture(tex); tex = {}; }
    }
    ssil_w_ = 0;
    ssil_h_ = 0;
}

void SSIL_RD::render(RenderContext* ctx,
                     RHITextureHandle color_tex,
                     RHITextureHandle depth_tex,
                     RHITextureHandle normal_roughness_tex,
                     const PostProcessParams& params,
                     int viewport_w, int viewport_h)
{
    if (!initialized_ || params.ssil_enabled == 0) return;

    // 1. SSIL 重要性采样 pass
    // 从半分辨率深度/法线重建间接光照
    // 使用多方向重要性采样 + 随机旋转

    // 2. 双边滤波平滑
    // 使用深度感知边缘保持模糊
}

} // namespace gryce_engine::render