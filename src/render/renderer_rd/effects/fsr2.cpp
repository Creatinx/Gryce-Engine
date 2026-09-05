#include "render/renderer_rd/effects/fsr2.h"
#include "render/render_context.h"
#include "render/mesh.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::render {

bool FSR2_RD::init(RenderContext* ctx) {
    if (initialized_) return true;
    ctx_ = ctx;
    initialized_ = true;
    return true;
}

void FSR2_RD::destroy() {
    if (!ctx_) return;
    destroy_targets();
    initialized_ = false;
}

bool FSR2_RD::create_targets(int render_w, int render_h, int output_w, int output_h) {
    destroy_targets();

    render_w_ = render_w;
    render_h_ = render_h;
    output_w_ = output_w;
    output_h_ = output_h;

    output_tex_ = ctx_->create_texture();
    ITexture* tex = ctx_->texture(output_tex_);
    if (!output_tex_.is_valid() || !tex ||
        !tex->create(TextureFormat::RGBA16F, output_w, output_h, nullptr)) {
        return false;
    }
    tex->set_filter(TextureFilter::Linear, TextureFilter::Linear);
    tex->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

    output_fbo_ = ctx_->create_framebuffer();
    IFramebuffer* fbo = ctx_->framebuffer(output_fbo_);
    if (!output_fbo_.is_valid() || !fbo || !fbo->create(output_w, output_h)) return false;
    fbo->attach_color_texture(tex);
    if (!fbo->is_complete()) return false;

    return true;
}

void FSR2_RD::destroy_targets() {
    if (!ctx_) return;
    if (output_tex_.is_valid()) { ctx_->destroy_texture(output_tex_); output_tex_ = {}; }
    if (output_fbo_.is_valid()) { ctx_->destroy_framebuffer(output_fbo_); output_fbo_ = {}; }
}

void FSR2_RD::render(RenderContext* ctx,
                     RHITextureHandle color_tex,
                     RHITextureHandle depth_tex,
                     RHITextureHandle motion_vectors_tex,
                     RHITextureHandle exposure_tex,
                     const PostProcessParams& params,
                     float jitter_x, float jitter_y)
{
    if (!initialized_ || params.fsr2_enabled == 0) return;

    // FSR2 渲染流程：
    // 1. EASU（边缘自适应上采样）- 将低分辨率渲染上采样到输出分辨率
    // 2. RCAS（鲁棒对比度自适应锐化）- 锐化最终输出

    // EASU Pass:
    // ctx->set_framebuffer(output_fbo_);
    // ctx->set_texture(fsr2_easu_shader_, color_tex, ...);
    // ctx->set_uniform_vec2(fsr2_easu_shader_, "uJitter", jitter_x, jitter_y);
    // ctx->draw_mesh(fullscreen_mesh_, fsr2_easu_shader_);

    // RCAS Pass:
    // ctx->set_shader(fsr2_rcas_shader_);
    // ctx->set_uniform_float(fsr2_rcas_shader_, "uSharpness", params.fsr2_sharpness);
    // ctx->draw_mesh(fullscreen_mesh_, fsr2_rcas_shader_);
}

void FSR2_RD::get_jitter(int frame_index, int render_w, int render_h,
                         float& jitter_x, float& jitter_y) {
    // Halton 序列抖动（与 Godot TAA 一致）
    const float halton_2[8] = {0.0f, 0.5f, 0.25f, 0.75f, 0.125f, 0.625f, 0.375f, 0.875f};
    const float halton_3[8] = {0.0f, 0.333f, 0.667f, 0.111f, 0.444f, 0.778f, 0.222f, 0.556f};
    int idx = frame_index % 8;
    jitter_x = (halton_2[idx] - 0.5f) / render_w;
    jitter_y = (halton_3[idx] - 0.5f) / render_h;
}

} // namespace gryce_engine::render