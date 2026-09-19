#include "render/renderer_rd/effects/ssil.h"
#include "render/render_context.h"
#include "render/mesh.h"
#include "render/texture.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::render {

bool SSIL_RD::init(RenderContext* ctx, const std::string& shader_dir) {
    if (initialized_) return true;
    ctx_ = ctx;

    // 命名式加载：由 resolver 按当前 API 解析 GL/vulkan 变体。
    // 当前只提供了 GL 变体；Vulkan 下 shaderc 会因方言差异编译失败，
    // 此时把 SSIL 标记为不可用（render() 直接跳过），不影响其它效果。
    auto load_shader = [&](const char* name, RHIShaderHandle& out) {
        out = ctx->create_shader();
        if (!out.is_valid()) return;
        IShader* s = ctx->shader(out);
        if (s && !s->load_program(name, shader_dir, nullptr, true, true)) {
            GLOG_WARN("SSIL_RD: failed to load shader '{}'", name);
        }
    };
    load_shader("ssil_trace", ssil_shader_);
    load_shader("ssil_blur", ssil_blur_shader_);

    IShader* trace = ctx->shader(ssil_shader_);
    IShader* blur = ctx->shader(ssil_blur_shader_);
    if (!trace || !trace->is_valid() || !blur || !blur->is_valid()) {
        GLOG_WARN("SSIL_RD: shaders unavailable, SSIL disabled");
        if (ssil_shader_.is_valid()) { ctx->destroy_shader(ssil_shader_); ssil_shader_ = {}; }
        if (ssil_blur_shader_.is_valid()) { ctx->destroy_shader(ssil_blur_shader_); ssil_blur_shader_ = {}; }
        return false;
    }

    // 全屏三角形（position + uv），与 gtao / ssr 等后处理 pass 一致
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
    if (!fullscreen_mesh_.is_valid()) {
        GLOG_WARN("SSIL_RD: fullscreen mesh creation failed, SSIL disabled");
        destroy();
        return false;
    }

    initialized_ = true;
    return true;
}

void SSIL_RD::destroy() {
    if (!ctx_) return;
    destroy_targets();
    if (ssil_shader_.is_valid()) { ctx_->destroy_shader(ssil_shader_); ssil_shader_ = {}; }
    if (ssil_blur_shader_.is_valid()) { ctx_->destroy_shader(ssil_blur_shader_); ssil_blur_shader_ = {}; }
    if (fullscreen_mesh_.is_valid()) { ctx_->destroy_mesh(fullscreen_mesh_); fullscreen_mesh_ = {}; }
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
                     const math::Matrix4f& view_matrix,
                     const math::Vector3f& camera_pos,
                     float near_plane,
                     float far_plane,
                     float tan_half_fov,
                     float aspect,
                     float intensity,
                     float radius,
                     int steps,
                     int viewport_w, int viewport_h)
{
    if (!initialized_ || !ctx) return;
    if (!ssil_shader_.is_valid() || !ssil_blur_shader_.is_valid()) return;
    if (!fullscreen_mesh_.is_valid()) return;
    if (!color_tex.is_valid() || !depth_tex.is_valid() || !normal_roughness_tex.is_valid()) return;

    // 窗口 resize 后重建目标
    if (ssil_w_ != viewport_w / 2 || ssil_h_ != viewport_h / 2) {
        if (!create_targets(viewport_w, viewport_h)) return;
    }

    if (steps < 2) steps = 2;
    if (steps > 32) steps = 32;

    ctx->set_depth_test(false);
    ctx->set_depth_write(false);
    ctx->set_cull_face(CullMode::None);
    ctx->set_blend(false);

    // ---- Pass 1: 半球重要性采样（半分辨率） ----
    ctx->set_framebuffer(ssil_fbo_[0]);
    ctx->set_viewport(0, 0, ssil_w_, ssil_h_);
    ctx->set_shader(ssil_shader_);   // 先绑 shader 再写采样器 uniform

    ctx->set_texture(ssil_shader_, color_tex, TextureSlots::kTonemapHDR, "uColorTex");
    ctx->set_uniform_int(ssil_shader_, "uColorTex", TextureSlots::kTonemapHDR);
    ctx->set_texture_raw_depth(ssil_shader_, depth_tex, TextureSlots::kPBRShadowDepth, "uDepthTex");
    ctx->set_uniform_int(ssil_shader_, "uDepthTex", TextureSlots::kPBRShadowDepth);
    ctx->set_texture(ssil_shader_, normal_roughness_tex, TextureSlots::kPBRShadowDepth1,
                     "uNormalRoughTex");
    ctx->set_uniform_int(ssil_shader_, "uNormalRoughTex", TextureSlots::kPBRShadowDepth1);

    ctx->set_uniform_mat4(ssil_shader_, "uView", view_matrix);
    ctx->set_uniform_vec3(ssil_shader_, "uCameraPos", camera_pos);
    ctx->set_uniform_vec2(ssil_shader_, "uScreenSize",
                          math::Vector2f(static_cast<float>(ssil_w_), static_cast<float>(ssil_h_)));
    ctx->set_uniform_float(ssil_shader_, "uSSILNear", near_plane);
    ctx->set_uniform_float(ssil_shader_, "uSSILFar", far_plane);
    ctx->set_uniform_float(ssil_shader_, "uSSILTanHalfFov", tan_half_fov);
    ctx->set_uniform_float(ssil_shader_, "uSSILAspect", aspect);
    ctx->set_uniform_float(ssil_shader_, "uSSILRadius", radius);
    ctx->set_uniform_float(ssil_shader_, "uSSILIntensity", intensity);
    ctx->set_uniform_int(ssil_shader_, "uSSILSteps", steps);
    ctx->draw_mesh(fullscreen_mesh_, ssil_shader_);

    // ---- Pass 2: 深度感知双边模糊（去噪，半分辨率 ping-pong） ----
    ctx->set_framebuffer(ssil_fbo_[1]);
    ctx->set_viewport(0, 0, ssil_w_, ssil_h_);
    ctx->set_shader(ssil_blur_shader_);
    ctx->set_texture(ssil_blur_shader_, ssil_tex_[0], TextureSlots::kSSILTexture, "uSSILTex");
    ctx->set_uniform_int(ssil_blur_shader_, "uSSILTex", TextureSlots::kSSILTexture);
    ctx->set_texture_raw_depth(ssil_blur_shader_, depth_tex, TextureSlots::kPBRShadowDepth,
                               "uDepthTex");
    ctx->set_uniform_int(ssil_blur_shader_, "uDepthTex", TextureSlots::kPBRShadowDepth);
    ctx->set_uniform_float(ssil_blur_shader_, "uSSILNear", near_plane);
    ctx->set_uniform_float(ssil_blur_shader_, "uSSILFar", far_plane);
    ctx->draw_mesh(fullscreen_mesh_, ssil_blur_shader_);

    // 输出取模糊后的那一张
    ping_ = 1;

    ctx->set_depth_test(true);
    ctx->set_depth_write(true);
}

} // namespace gryce_engine::render
