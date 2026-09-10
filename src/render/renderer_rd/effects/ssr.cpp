#include "render/renderer_rd/effects/ssr.h"
#include "render/render_context.h"
#include "render/texture.h"
#include "render/framebuffer.h"
#include "render/mesh.h"
#include "render/shader.h"
#include "utils/glog/glog_lib.h"

#include <cstdio>

namespace gryce_engine::render {

bool SSR_RD::init(RenderContext* ctx, const std::string& shader_dir) {
    if (initialized_) return true;
    ctx_ = ctx;

    // 加载 SSR 四段 shader（每个 pass 独立的全屏 VS + FS，由 shader 系统
    // 按名解析：项目磁盘 → bundle → 引擎默认 shader 目录）
    auto load_shader = [&](const char* name, RHIShaderHandle& out) {
        out = ctx->create_shader();
        if (!out.is_valid()) return;
        IShader* s = ctx->shader(out);
        if (s && !s->load_program(name, shader_dir, nullptr, true, true)) {
            GLOG_WARN("SSR_RD: failed to load shader '{}'", name);
        }
    };
    load_shader("ssr_hiz", ssr_hiz_shader_);
    load_shader("ssr_trace", ssr_trace_shader_);
    load_shader("ssr_blur", ssr_blur_shader_);
    load_shader("ssr_composite", ssr_composite_shader_);

    // 创建全屏三角形 mesh（position + uv，与 gtao/bloom 等后处理 pass 一致）
    fullscreen_mesh_ = ctx->create_mesh();
    if (fullscreen_mesh_.is_valid()) {
        IMesh* mesh = ctx->mesh(fullscreen_mesh_);
        if (mesh) {
            struct Vertex { float x, y; float u, v; };
            Vertex verts[] = {
                {-1.0f, -1.0f, 0.0f, 0.0f},
                { 3.0f, -1.0f, 2.0f, 0.0f},
                {-1.0f,  3.0f, 0.0f, 2.0f}
            };
            VertexLayout layout;
            layout.stride = sizeof(Vertex);
            layout.attributes = {
                {0, VertexType::Float2, false, 0},
                {1, VertexType::Float2, false, 2 * sizeof(float)}
            };
            mesh->set_layout(layout);
            mesh->upload_vertices(verts, sizeof(verts), 3);
        }
    }

    initialized_ = true;
    return true;
}

void SSR_RD::destroy() {
    if (!ctx_) return;
    destroy_targets();
    if (ssr_hiz_shader_.is_valid()) { ctx_->destroy_shader(ssr_hiz_shader_); ssr_hiz_shader_ = {}; }
    if (ssr_trace_shader_.is_valid()) { ctx_->destroy_shader(ssr_trace_shader_); ssr_trace_shader_ = {}; }
    if (ssr_blur_shader_.is_valid()) { ctx_->destroy_shader(ssr_blur_shader_); ssr_blur_shader_ = {}; }
    if (ssr_composite_shader_.is_valid()) { ctx_->destroy_shader(ssr_composite_shader_); ssr_composite_shader_ = {}; }
    if (fullscreen_mesh_.is_valid()) { ctx_->destroy_mesh(fullscreen_mesh_); fullscreen_mesh_ = {}; }
    initialized_ = false;
}

bool SSR_RD::create_targets(int width, int height) {
    destroy_targets();

    ssr_w_ = std::max(16, width);
    ssr_h_ = std::max(16, height);

    // SSR 光线步进输出（全分辨率 RGBA16F）
    ssr_tex_ = ctx_->create_texture();
    ITexture* tex = ctx_->texture(ssr_tex_);
    if (!ssr_tex_.is_valid() || !tex ||
        !tex->create(TextureFormat::RGBA16F, ssr_w_, ssr_h_, nullptr)) {
        return false;
    }
    tex->set_filter(TextureFilter::Linear, TextureFilter::Linear);
    tex->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

    ssr_fbo_ = ctx_->create_framebuffer();
    IFramebuffer* fbo = ctx_->framebuffer(ssr_fbo_);
    if (!ssr_fbo_.is_valid() || !fbo || !fbo->create(ssr_w_, ssr_h_)) return false;
    fbo->attach_color_texture(tex);
    if (!fbo->is_complete()) return false;

    // SSR 双边模糊输出（全分辨率 RGBA16F）
    ssr_tex_blur_ = ctx_->create_texture();
    tex = ctx_->texture(ssr_tex_blur_);
    if (!ssr_tex_blur_.is_valid() || !tex ||
        !tex->create(TextureFormat::RGBA16F, ssr_w_, ssr_h_, nullptr)) {
        return false;
    }
    tex->set_filter(TextureFilter::Linear, TextureFilter::Linear);
    tex->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

    ssr_blur_fbo_ = ctx_->create_framebuffer();
    fbo = ctx_->framebuffer(ssr_blur_fbo_);
    if (!ssr_blur_fbo_.is_valid() || !fbo || !fbo->create(ssr_w_, ssr_h_)) return false;
    fbo->attach_color_texture(tex);
    if (!fbo->is_complete()) return false;

    // HiZ 金字塔（R32F，2x2 最小深度下采样；level 0 = 半分辨率，逐级减半）
    int w = std::max(1, ssr_w_ / 2);
    int h = std::max(1, ssr_h_ / 2);
    for (int i = 0; i < k_ssr_mip_count; ++i) {
        hiz_w_[i] = w;
        hiz_h_[i] = h;

        hiz_tex_[i] = ctx_->create_texture();
        ITexture* htex = ctx_->texture(hiz_tex_[i]);
        if (!hiz_tex_[i].is_valid() || !htex ||
            !htex->create(TextureFormat::R32F, w, h, nullptr)) {
            GLOG_ERROR("SSR_RD: HiZ texture level {} failed ({}x{})", i, w, h);
            return false;
        }
        // HiZ 必须用 Nearest，避免线性过滤在两个层之间插值出错误的最小深度
        htex->set_filter(TextureFilter::Nearest, TextureFilter::Nearest);
        htex->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

        hiz_fbo_[i] = ctx_->create_framebuffer();
        IFramebuffer* hfbo = ctx_->framebuffer(hiz_fbo_[i]);
        if (!hiz_fbo_[i].is_valid() || !hfbo || !hfbo->create(w, h)) return false;
        hfbo->attach_color_texture(htex);
        if (!hfbo->is_complete()) return false;

        w = std::max(1, w / 2);
        h = std::max(1, h / 2);
    }

    targets_valid_ = true;
    return true;
}

void SSR_RD::destroy_targets() {
    if (!ctx_) return;
    for (auto& fbo : hiz_fbo_) {
        if (fbo.is_valid()) { ctx_->destroy_framebuffer(fbo); fbo = {}; }
    }
    for (auto& tex : hiz_tex_) {
        if (tex.is_valid()) { ctx_->destroy_texture(tex); tex = {}; }
    }
    if (ssr_tex_.is_valid()) { ctx_->destroy_texture(ssr_tex_); ssr_tex_ = {}; }
    if (ssr_fbo_.is_valid()) { ctx_->destroy_framebuffer(ssr_fbo_); ssr_fbo_ = {}; }
    if (ssr_tex_blur_.is_valid()) { ctx_->destroy_texture(ssr_tex_blur_); ssr_tex_blur_ = {}; }
    if (ssr_blur_fbo_.is_valid()) { ctx_->destroy_framebuffer(ssr_blur_fbo_); ssr_blur_fbo_ = {}; }
    for (auto& d : hiz_w_) d = 0;
    for (auto& d : hiz_h_) d = 0;
    targets_valid_ = false;
}

void SSR_RD::render(RenderContext* ctx,
                    RHITextureHandle color_tex,
                    RHITextureHandle depth_tex,
                    RHITextureHandle normal_roughness_tex,
                    RHIFramebufferHandle output_fbo,
                    const math::Matrix4f& view_matrix,
                    const math::Vector3f& camera_pos,
                    const PostProcessParams& params,
                    int viewport_w, int viewport_h)
{
    if (!initialized_ || !targets_valid_ || params.ssr_enabled == 0) return;
    if (!ssr_hiz_shader_.is_valid() || !ssr_trace_shader_.is_valid() ||
        !ssr_blur_shader_.is_valid() || !ssr_composite_shader_.is_valid() ||
        !fullscreen_mesh_.is_valid() || !output_fbo.is_valid()) return;
    // 屏幕空间反射需要法线/粗糙度缓冲（前向路径无 G-buffer 时跳过）
    if (!normal_roughness_tex.is_valid()) return;

    // 窗口 resize 后重建目标
    if (ssr_w_ != viewport_w || ssr_h_ != viewport_h) {
        if (!create_targets(viewport_w, viewport_h)) return;
    }

    ctx->set_depth_test(false);
    ctx->set_depth_write(false);
    ctx->set_cull_face(CullMode::None);
    ctx->set_blend(false);

    // 同步后处理参数（uSSR* 相机参数/粗糙度上限/厚度等）
    for (RHIShaderHandle h : {ssr_hiz_shader_, ssr_trace_shader_,
                              ssr_blur_shader_, ssr_composite_shader_}) {
        IShader* s = ctx->shader(h);
        if (s) s->set_post_process_params(params);
    }

    // ---- Pass 1: HiZ 金字塔构建 ----
    // level 0 输入为全分辨率深度，后续各级输入为上一级 HiZ
    for (int i = 0; i < k_ssr_mip_count; ++i) {
        ctx->set_framebuffer(hiz_fbo_[i]);
        ctx->set_viewport(0, 0, hiz_w_[i], hiz_h_[i]);

        const RHITextureHandle input = (i == 0) ? depth_tex : hiz_tex_[i - 1];
        if (i == 0) {
            ctx->set_texture_raw_depth(ssr_hiz_shader_, input, TextureSlots::kTonemapHDR, "uInput");
        } else {
            ctx->set_texture(ssr_hiz_shader_, input, TextureSlots::kTonemapHDR, "uInput");
        }
        ctx->set_uniform_int(ssr_hiz_shader_, "uInput", TextureSlots::kTonemapHDR);

        const float in_w = (i == 0) ? static_cast<float>(viewport_w)
                                    : static_cast<float>(hiz_w_[i - 1]);
        const float in_h = (i == 0) ? static_cast<float>(viewport_h)
                                    : static_cast<float>(hiz_h_[i - 1]);
        ctx->set_uniform_vec2(ssr_hiz_shader_, "uTexelSize",
                              math::Vector2f(1.0f / in_w, 1.0f / in_h));
        ctx->draw_mesh(fullscreen_mesh_, ssr_hiz_shader_);
    }

    // ---- Pass 2: SSR 光线步进（全分辨率 → ssr_tex_） ----
    ctx->set_framebuffer(ssr_fbo_);
    ctx->set_viewport(0, 0, ssr_w_, ssr_h_);

    ctx->set_texture(ssr_trace_shader_, color_tex, TextureSlots::kTonemapHDR, "uColorTex");
    ctx->set_uniform_int(ssr_trace_shader_, "uColorTex", TextureSlots::kTonemapHDR);
    ctx->set_texture_raw_depth(ssr_trace_shader_, depth_tex, TextureSlots::kPBRShadowDepth, "uDepthTex");
    ctx->set_uniform_int(ssr_trace_shader_, "uDepthTex", TextureSlots::kPBRShadowDepth);
    ctx->set_texture(ssr_trace_shader_, normal_roughness_tex, TextureSlots::kPBRShadowDepth1, "uNormalRoughTex");
    ctx->set_uniform_int(ssr_trace_shader_, "uNormalRoughTex", TextureSlots::kPBRShadowDepth1);
    for (int i = 0; i < k_ssr_mip_count; ++i) {
        const int slot = TextureSlots::kSSRHiZ + i;
        char name[16];
        std::snprintf(name, sizeof(name), "uHiZ%d", i);
        ctx->set_texture(ssr_trace_shader_, hiz_tex_[i], slot, name);
        ctx->set_uniform_int(ssr_trace_shader_, name, slot);
    }

    ctx->set_uniform_mat4(ssr_trace_shader_, "uView", view_matrix);
    ctx->set_uniform_vec3(ssr_trace_shader_, "uCameraPos", camera_pos);
    ctx->set_uniform_vec2(ssr_trace_shader_, "uScreenSize",
                          math::Vector2f(static_cast<float>(ssr_w_), static_cast<float>(ssr_h_)));
    ctx->draw_mesh(fullscreen_mesh_, ssr_trace_shader_);

    // ---- Pass 3: 双边模糊（ssr_tex_ → ssr_tex_blur_） ----
    ctx->set_framebuffer(ssr_blur_fbo_);
    ctx->set_viewport(0, 0, ssr_w_, ssr_h_);

    ctx->set_texture(ssr_blur_shader_, ssr_tex_, TextureSlots::kSSRTexture, "uTexture");
    ctx->set_uniform_int(ssr_blur_shader_, "uTexture", TextureSlots::kSSRTexture);
    ctx->set_texture_raw_depth(ssr_blur_shader_, depth_tex, TextureSlots::kPBRShadowDepth, "uDepthTexture");
    ctx->set_uniform_int(ssr_blur_shader_, "uDepthTexture", TextureSlots::kPBRShadowDepth);
    ctx->set_uniform_float(ssr_blur_shader_, "uSSRBilateralFilter", params.ssr_bilateral_filter);
    ctx->draw_mesh(fullscreen_mesh_, ssr_blur_shader_);

    // ---- Pass 4: 合成（HDR 颜色 + SSR 反射 → output_fbo） ----
    ctx->set_framebuffer(output_fbo);
    ctx->set_viewport(0, 0, viewport_w, viewport_h);

    ctx->set_texture(ssr_composite_shader_, color_tex, TextureSlots::kTonemapHDR, "uColorTex");
    ctx->set_uniform_int(ssr_composite_shader_, "uColorTex", TextureSlots::kTonemapHDR);
    ctx->set_texture(ssr_composite_shader_, ssr_tex_blur_, TextureSlots::kSSRTexture, "uSSRTex");
    ctx->set_uniform_int(ssr_composite_shader_, "uSSRTex", TextureSlots::kSSRTexture);
    ctx->draw_mesh(fullscreen_mesh_, ssr_composite_shader_);

    ctx->set_depth_test(true);
    ctx->set_depth_write(true);
}

} // namespace gryce_engine::render
