#include "vsm_esm_shadow.h"

#include "render/render_context.h"
#include "render/shader.h"
#include "render/texture.h"
#include "render/framebuffer.h"
#include "resources/resource_path.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::render {

// ---------------------------------------------------------------------------
// 工具：创建 RGBA16F 纹理 + 附加到 FBO
// ---------------------------------------------------------------------------
static bool create_rgba16f_target(RenderContext* ctx, int width, int height,
                                  RHITextureHandle& out_tex, RHIFramebufferHandle& out_fbo) {
    out_tex = ctx->create_texture();
    ITexture* tex_ptr = ctx->texture(out_tex);
    if (!out_tex.is_valid() || !tex_ptr ||
        !tex_ptr->create(TextureFormat::RGBA16F, width, height, nullptr)) {
        GLOG_ERROR("VSMESMShadow: failed to create RGBA16F texture ({}x{})", width, height);
        return false;
    }
    tex_ptr->set_filter(TextureFilter::Linear, TextureFilter::Linear);
    tex_ptr->set_wrap(TextureWrap::ClampToBorder, TextureWrap::ClampToBorder);

    out_fbo = ctx->create_framebuffer();
    IFramebuffer* fbo_ptr = ctx->framebuffer(out_fbo);
    if (!out_fbo.is_valid() || !fbo_ptr || !fbo_ptr->create(width, height)) {
        GLOG_ERROR("VSMESMShadow: failed to create framebuffer ({}x{})", width, height);
        return false;
    }
    fbo_ptr->attach_color_texture(tex_ptr);
    if (!fbo_ptr->is_complete()) {
        GLOG_ERROR("VSMESMShadow: framebuffer incomplete ({}x{})", width, height);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 工具：销毁 RGBA16F 纹理 + FBO
// ---------------------------------------------------------------------------
static void destroy_target(RenderContext* ctx, RHITextureHandle& tex, RHIFramebufferHandle& fbo) {
    if (fbo.is_valid()) {
        ctx->destroy_framebuffer(fbo);
        fbo = RHIFramebufferHandle{};
    }
    if (tex.is_valid()) {
        ctx->destroy_texture(tex);
        tex = RHITextureHandle{};
    }
}

// ===========================================================================
// VSMESMShadow 实现
// ===========================================================================

bool VSMESMShadow::init(RenderContext* ctx) {
    if (!ctx) return false;
    ctx_ = ctx;

    const std::string shader_dir = resources::ResourcePath::resolve("res:/shaders");

    // ---- VSM 渲染 shader ----
    // 使用 shadow_vsm.vert + shadow_vsm.frag，输出到 RGBA16F FBO
    {
        vsm_shader_ = ctx->create_shader();
        IShader* s = ctx->shader(vsm_shader_);
        if (!vsm_shader_.is_valid() || !s ||
            !s->load_program("shadow_vsm", shader_dir, nullptr, true, false)) {
            GLOG_WARN("VSMESMShadow: shadow_vsm shader unavailable, VSM disabled");
            if (vsm_shader_.is_valid()) {
                ctx->destroy_shader(vsm_shader_);
                vsm_shader_ = RHIShaderHandle{};
            }
        }
    }

    // ---- ESM 渲染 shader ----
    {
        esm_shader_ = ctx->create_shader();
        IShader* s = ctx->shader(esm_shader_);
        if (!esm_shader_.is_valid() || !s ||
            !s->load_program("shadow_esm", shader_dir, nullptr, true, false)) {
            GLOG_WARN("VSMESMShadow: shadow_esm shader unavailable, ESM disabled");
            if (esm_shader_.is_valid()) {
                ctx->destroy_shader(esm_shader_);
                esm_shader_ = RHIShaderHandle{};
            }
        }
    }

    // ---- VSM 模糊 shader（分离式高斯） ----
    {
        vsm_blur_h_shader_ = ctx->create_shader();
        IShader* s = ctx->shader(vsm_blur_h_shader_);
        if (!vsm_blur_h_shader_.is_valid() || !s ||
            !s->load_program("vsm_blur", shader_dir, nullptr, true, true)) {
            GLOG_WARN("VSMESMShadow: vsm_blur shader unavailable, VSM blur disabled");
            if (vsm_blur_h_shader_.is_valid()) {
                ctx->destroy_shader(vsm_blur_h_shader_);
                vsm_blur_h_shader_ = RHIShaderHandle{};
            }
        }
    }

    if (!vsm_shader_.is_valid() && !esm_shader_.is_valid()) {
        GLOG_ERROR("VSMESMShadow: no VSM or ESM shaders loaded");
        return false;
    }

    initialized_ = true;
    return true;
}

void VSMESMShadow::destroy() {
    if (!ctx_) return;

    destroy_vsm_targets();

    // 销毁临时模糊缓冲
    if (blur_temp_fbo_.is_valid()) {
        ctx_->destroy_framebuffer(blur_temp_fbo_);
        blur_temp_fbo_ = RHIFramebufferHandle{};
    }
    if (blur_temp_tex_.is_valid()) {
        ctx_->destroy_texture(blur_temp_tex_);
        blur_temp_tex_ = RHITextureHandle{};
    }

    if (vsm_shader_.is_valid()) {
        ctx_->destroy_shader(vsm_shader_);
        vsm_shader_ = RHIShaderHandle{};
    }
    if (esm_shader_.is_valid()) {
        ctx_->destroy_shader(esm_shader_);
        esm_shader_ = RHIShaderHandle{};
    }
    if (vsm_blur_h_shader_.is_valid()) {
        ctx_->destroy_shader(vsm_blur_h_shader_);
        vsm_blur_h_shader_ = RHIShaderHandle{};
    }

    ctx_ = nullptr;
    initialized_ = false;
}

bool VSMESMShadow::create_vsm_targets(int cascade, int width, int height) {
    if (!ctx_ || cascade < 0 || cascade >= k_max_cascades) return false;

    // 释放旧目标
    destroy_target(ctx_, vsm_tex_[cascade], vsm_fbo_[cascade]);

    if (!create_rgba16f_target(ctx_, width, height, vsm_tex_[cascade], vsm_fbo_[cascade])) {
        GLOG_ERROR("VSMESMShadow: create_vsm_targets cascade {} failed ({}x{})", cascade, width, height);
        return false;
    }

    // 创建临时模糊缓冲（仅第 0 级联创建，所有级联复用）
    if (blur_temp_tex_.is_valid() == false) {
        destroy_target(ctx_, blur_temp_tex_, blur_temp_fbo_);
        if (!create_rgba16f_target(ctx_, width, height, blur_temp_tex_, blur_temp_fbo_)) {
            GLOG_WARN("VSMESMShadow: blur temp target creation failed");
        }
    }

    return true;
}

void VSMESMShadow::destroy_vsm_targets() {
    if (!ctx_) return;
    for (int i = 0; i < k_max_cascades; ++i) {
        destroy_target(ctx_, vsm_tex_[i], vsm_fbo_[i]);
    }
}

void VSMESMShadow::blur_vsm(RenderContext* ctx, int cascade, int width, int height) {
    if (!ctx || !vsm_blur_h_shader_.is_valid() || !blur_temp_fbo_.is_valid()) return;
    if (cascade < 0 || cascade >= k_max_cascades) return;
    if (!vsm_tex_[cascade].is_valid() || !blur_temp_tex_.is_valid()) return;

    // 临时缓冲尺寸必须至少与当前级联一样大；若不够则重建
    ITexture* temp_tex_ptr = ctx->texture(blur_temp_tex_);
    IFramebuffer* temp_fbo_ptr = ctx->framebuffer(blur_temp_fbo_);
    if (temp_tex_ptr && (temp_tex_ptr->width() < width || temp_tex_ptr->height() < height)) {
        // 重建临时缓冲
        destroy_target(ctx_, blur_temp_tex_, blur_temp_fbo_);
        if (!create_rgba16f_target(ctx_, width, height, blur_temp_tex_, blur_temp_fbo_)) {
            GLOG_WARN("VSMESMShadow: blur temp target resize failed");
            return;
        }
        temp_tex_ptr = ctx->texture(blur_temp_tex_);
        temp_fbo_ptr = ctx->framebuffer(blur_temp_fbo_);
    }

    // ---- Pass 1: 水平模糊：vsm_tex → blur_temp_tex ----
    ctx->set_framebuffer(blur_temp_fbo_);
    ctx->set_viewport(0, 0, width, height);
    ctx->clear(0.0f, 0.0f, 0.0f, 0.0f);
    // 绑定 VSM 纹理作为输入
    ITexture* vsm_tex_ptr = ctx->texture(vsm_tex_[cascade]);
    if (vsm_tex_ptr) vsm_tex_ptr->bind(0);
    ctx->set_texture(vsm_blur_h_shader_, vsm_tex_[cascade], 0, "uVSMTexture");
    ctx->set_uniform_vec2(vsm_blur_h_shader_, "uBlurDirection",
                          math::Vector2f(1.0f / static_cast<float>(width), 0.0f));
    // 使用全屏四边形绘制
    if (fullscreen_mesh_.is_valid()) {
        ctx->draw_mesh(fullscreen_mesh_, vsm_blur_h_shader_);
    }

    // ---- Pass 2: 垂直模糊：blur_temp_tex → vsm_tex ----
    ctx->set_framebuffer(vsm_fbo_[cascade]);
    ctx->set_viewport(0, 0, width, height);
    ctx->clear(0.0f, 0.0f, 0.0f, 0.0f);
    if (temp_tex_ptr) temp_tex_ptr->bind(0);
    ctx->set_texture(vsm_blur_h_shader_, blur_temp_tex_, 0, "uVSMTexture");
    ctx->set_uniform_vec2(vsm_blur_h_shader_, "uBlurDirection",
                          math::Vector2f(0.0f, 1.0f / static_cast<float>(height)));
    if (fullscreen_mesh_.is_valid()) {
        ctx->draw_mesh(fullscreen_mesh_, vsm_blur_h_shader_);
    }

    ctx->set_framebuffer(RHIFramebufferHandle{});
}

} // namespace gryce_engine::render