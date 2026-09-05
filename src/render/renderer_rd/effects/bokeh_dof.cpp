#include "render/renderer_rd/effects/bokeh_dof.h"
#include "render/render_context.h"
#include "render/mesh.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::render {

bool BokehDOF_RD::init(RenderContext* ctx) {
    if (initialized_) return true;
    ctx_ = ctx;
    initialized_ = true;
    return true;
}

void BokehDOF_RD::destroy() {
    if (!ctx_) return;
    destroy_targets();
    initialized_ = false;
}

bool BokehDOF_RD::create_targets(int width, int height) {
    destroy_targets();

    dof_w_ = width;
    dof_h_ = height;
    int half_w = std::max(16, width / 2);
    int half_h = std::max(16, height / 2);

    // 半分辨率降采样缓冲
    {
        half_tex_ = ctx_->create_texture();
        ITexture* tex = ctx_->texture(half_tex_);
        if (!half_tex_.is_valid() || !tex ||
            !tex->create(TextureFormat::RGBA16F, half_w, half_h, nullptr)) {
            return false;
        }
        tex->set_filter(TextureFilter::Linear, TextureFilter::Linear);
        tex->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

        half_fbo_ = ctx_->create_framebuffer();
        IFramebuffer* fbo = ctx_->framebuffer(half_fbo_);
        if (!half_fbo_.is_valid() || !fbo || !fbo->create(half_w, half_h)) return false;
        fbo->attach_color_texture(tex);
        if (!fbo->is_complete()) return false;
    }

    // 模糊双缓冲（半分辨率）
    for (int i = 0; i < 2; ++i) {
        blur_tex_[i] = ctx_->create_texture();
        ITexture* tex = ctx_->texture(blur_tex_[i]);
        if (!blur_tex_[i].is_valid() || !tex ||
            !tex->create(TextureFormat::RGBA16F, half_w, half_h, nullptr)) {
            return false;
        }
        tex->set_filter(TextureFilter::Linear, TextureFilter::Linear);
        tex->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

        blur_fbo_[i] = ctx_->create_framebuffer();
        IFramebuffer* fbo = ctx_->framebuffer(blur_fbo_[i]);
        if (!blur_fbo_[i].is_valid() || !fbo || !fbo->create(half_w, half_h)) return false;
        fbo->attach_color_texture(tex);
        if (!fbo->is_complete()) return false;
    }

    // 全分辨率输出
    {
        dof_tex_ = ctx_->create_texture();
        ITexture* tex = ctx_->texture(dof_tex_);
        if (!dof_tex_.is_valid() || !tex ||
            !tex->create(TextureFormat::RGBA16F, width, height, nullptr)) {
            return false;
        }
        tex->set_filter(TextureFilter::Linear, TextureFilter::Linear);
        tex->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

        dof_fbo_ = ctx_->create_framebuffer();
        IFramebuffer* fbo = ctx_->framebuffer(dof_fbo_);
        if (!dof_fbo_.is_valid() || !fbo || !fbo->create(width, height)) return false;
        fbo->attach_color_texture(tex);
        if (!fbo->is_complete()) return false;
    }

    return true;
}

void BokehDOF_RD::destroy_targets() {
    if (!ctx_) return;
    if (half_tex_.is_valid()) { ctx_->destroy_texture(half_tex_); half_tex_ = {}; }
    if (half_fbo_.is_valid()) { ctx_->destroy_framebuffer(half_fbo_); half_fbo_ = {}; }
    for (auto& fbo : blur_fbo_) {
        if (fbo.is_valid()) { ctx_->destroy_framebuffer(fbo); fbo = {}; }
    }
    for (auto& tex : blur_tex_) {
        if (tex.is_valid()) { ctx_->destroy_texture(tex); tex = {}; }
    }
    if (dof_tex_.is_valid()) { ctx_->destroy_texture(dof_tex_); dof_tex_ = {}; }
    if (dof_fbo_.is_valid()) { ctx_->destroy_framebuffer(dof_fbo_); dof_fbo_ = {}; }
}

void BokehDOF_RD::render(RenderContext* ctx,
                         RHITextureHandle color_tex,
                         RHITextureHandle depth_tex,
                         const PostProcessParams& params,
                         int viewport_w, int viewport_h)
{
    if (!initialized_ || params.dof_enabled == 0) return;

    // 1. 半分辨率降采样 + CoC 计算
    // 从全分辨率 HDR 颜色 + 深度计算弥散圆（CoC）并降采样

    // 2. 水平方向六边形 Bokeh 模糊
    // 使用 CoC 加权扩散

    // 3. 垂直方向六边形 Bokeh 模糊
    // 使用 CoC 加权扩散

    // 4. 上采样合成到全分辨率
    // 与原始 HDR 颜色合成（远处用 Bokeh 结果，近处用原始结果）
}

} // namespace gryce_engine::render