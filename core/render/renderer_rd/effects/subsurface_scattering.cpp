#include "render/renderer_rd/effects/subsurface_scattering.h"
#include "render/render_context.h"
#include "render/texture.h"
#include "render/framebuffer.h"
#include "render/mesh.h"
#include "render/shader.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::render {

// ---------------------------------------------------------------------------
// init / destroy
// ---------------------------------------------------------------------------

bool SubsurfaceScattering_RD::init(RenderContext* ctx, const std::string& shader_dir) {
    if (initialized_) return true;
    ctx_ = ctx;

    // 加载 SSS shader - 使用 fog.vert 作为全屏顶点着色器
    std::string vs = shader_dir + "/fog.vert";

    // 水平模糊 shader
    std::string sss_h_fs = shader_dir + "/sss_horizontal.frag";
    sss_horizontal_shader_ = ctx->create_shader();
    if (sss_horizontal_shader_.is_valid()) {
        IShader* s = ctx->shader(sss_horizontal_shader_);
        if (s) {
            if (!s->load_program(vs.c_str(), sss_h_fs.c_str())) {
                GLOG_WARN("SubsurfaceScattering: failed to load horizontal shader");
            }
        }
    }

    // 垂直模糊 shader
    std::string sss_v_fs = shader_dir + "/sss_vertical.frag";
    sss_vertical_shader_ = ctx->create_shader();
    if (sss_vertical_shader_.is_valid()) {
        IShader* s = ctx->shader(sss_vertical_shader_);
        if (s) {
            if (!s->load_program(vs.c_str(), sss_v_fs.c_str())) {
                GLOG_WARN("SubsurfaceScattering: failed to load vertical shader");
            }
        }
    }

    // 合成 shader
    std::string sss_c_fs = shader_dir + "/sss_composite.frag";
    sss_composite_shader_ = ctx->create_shader();
    if (sss_composite_shader_.is_valid()) {
        IShader* s = ctx->shader(sss_composite_shader_);
        if (s) {
            if (!s->load_program(vs.c_str(), sss_c_fs.c_str())) {
                GLOG_WARN("SubsurfaceScattering: failed to load composite shader");
            }
        }
    }

    // 创建全屏 mesh
    fullscreen_mesh_ = ctx->create_mesh();
    if (fullscreen_mesh_.is_valid()) {
        IMesh* mesh = ctx->mesh(fullscreen_mesh_);
        if (mesh) {
            float verts[] = { -1.0f, -1.0f, 0.0f,  1.0f, -1.0f, 0.0f,  -1.0f, 1.0f, 0.0f,  1.0f, 1.0f, 0.0f };
            uint32_t indices[] = { 0, 1, 2, 2, 1, 3 };
            VertexLayout layout;
            layout.stride = 3 * sizeof(float);
            layout.attributes = { {0, VertexType::Float3, false, 0} };
            mesh->set_layout(layout);
            mesh->upload_vertices(verts, 3 * sizeof(float), 4);
            mesh->upload_indices(indices, sizeof(uint32_t), 6);
        }
    }

    initialized_ = true;
    return true;
}

void SubsurfaceScattering_RD::destroy() {
    if (!ctx_) return;
    destroy_targets();
    if (sss_horizontal_shader_.is_valid()) { ctx_->destroy_shader(sss_horizontal_shader_); sss_horizontal_shader_ = {}; }
    if (sss_vertical_shader_.is_valid()) { ctx_->destroy_shader(sss_vertical_shader_); sss_vertical_shader_ = {}; }
    if (sss_composite_shader_.is_valid()) { ctx_->destroy_shader(sss_composite_shader_); sss_composite_shader_ = {}; }
    if (fullscreen_mesh_.is_valid()) { ctx_->destroy_mesh(fullscreen_mesh_); fullscreen_mesh_ = {}; }
    initialized_ = false;
}

// ---------------------------------------------------------------------------
// targets
// ---------------------------------------------------------------------------

bool SubsurfaceScattering_RD::create_targets(int width, int height) {
    destroy_targets();

    sss_w_ = std::max(16, width);
    sss_h_ = std::max(16, height);

    for (int i = 0; i < 2; ++i) {
        sss_tex_[i] = ctx_->create_texture();
        ITexture* tex = ctx_->texture(sss_tex_[i]);
        if (!sss_tex_[i].is_valid() || !tex ||
            !tex->create(TextureFormat::RGBA16F, sss_w_, sss_h_, nullptr)) {
            return false;
        }
        tex->set_filter(TextureFilter::Linear, TextureFilter::Linear);
        tex->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

        sss_fbo_[i] = ctx_->create_framebuffer();
        IFramebuffer* fbo = ctx_->framebuffer(sss_fbo_[i]);
        if (!sss_fbo_[i].is_valid() || !fbo || !fbo->create(sss_w_, sss_h_)) return false;
        fbo->attach_color_texture(tex);
        if (!fbo->is_complete()) return false;
    }

    targets_valid_ = true;
    return true;
}

void SubsurfaceScattering_RD::destroy_targets() {
    if (!ctx_) return;
    for (auto& fbo : sss_fbo_) {
        if (fbo.is_valid()) { ctx_->destroy_framebuffer(fbo); fbo = {}; }
    }
    for (auto& tex : sss_tex_) {
        if (tex.is_valid()) { ctx_->destroy_texture(tex); tex = {}; }
    }
    targets_valid_ = false;
}

// ---------------------------------------------------------------------------
// render
// ---------------------------------------------------------------------------

void SubsurfaceScattering_RD::render(RenderContext* ctx,
                                     RHITextureHandle scene_color_tex,
                                     RHITextureHandle depth_tex,
                                     RHIFramebufferHandle output_fbo,
                                     const PostProcessParams& params,
                                     int width, int height) {
    if (!initialized_ || !targets_valid_) return;
    if (!sss_horizontal_shader_.is_valid() || !sss_vertical_shader_.is_valid() ||
        !sss_composite_shader_.is_valid() || !fullscreen_mesh_.is_valid()) return;

    // 确保目标大小匹配
    if (sss_w_ != width || sss_h_ != height) {
        create_targets(width, height);
    }

    ctx->set_depth_test(false);
    ctx->set_cull_face(CullMode::None);
    ctx->set_blend(false);

    // 同步后处理参数
    for (RHIShaderHandle h : {sss_horizontal_shader_, sss_vertical_shader_, sss_composite_shader_}) {
        IShader* s = ctx->shader(h);
        if (s) s->set_post_process_params(params);
    }

    // Pass 1: 水平模糊 → sss_tex_[0]
    ctx->set_framebuffer(sss_fbo_[0]);
    ctx->set_viewport(0, 0, sss_w_, sss_h_);
    ctx->set_texture(sss_horizontal_shader_, scene_color_tex, TextureSlots::kTonemapHDR, "uSceneColor");
    ctx->set_uniform_int(sss_horizontal_shader_, "uSceneColor", TextureSlots::kTonemapHDR);
    ctx->set_texture_raw_depth(sss_horizontal_shader_, depth_tex, TextureSlots::kTAAHistory, "uDepthTex");
    ctx->set_uniform_int(sss_horizontal_shader_, "uDepthTex", TextureSlots::kTAAHistory);
    ctx->push_command([h = sss_horizontal_shader_, w = static_cast<float>(width), hh = static_cast<float>(height)](IRenderBackend* backend) {
        IShader* s = backend->shader(h);
        if (s) s->set_vec2("uScreenSize", math::Vector2f(w, hh));
    });
    ctx->set_uniform_float(sss_horizontal_shader_, "uSSSStrength", params.sss_strength);
    ctx->set_uniform_float(sss_horizontal_shader_, "uSSSScale", params.sss_scale);
    ctx->draw_mesh(fullscreen_mesh_, sss_horizontal_shader_);

    // Pass 2: 垂直模糊 → sss_tex_[1]
    ctx->set_framebuffer(sss_fbo_[1]);
    ctx->set_viewport(0, 0, sss_w_, sss_h_);
    ctx->set_texture(sss_vertical_shader_, sss_tex_[0], TextureSlots::kTonemapHDR, "uSceneColor");
    ctx->set_uniform_int(sss_vertical_shader_, "uSceneColor", TextureSlots::kTonemapHDR);
    ctx->set_texture_raw_depth(sss_vertical_shader_, depth_tex, TextureSlots::kTAAHistory, "uDepthTex");
    ctx->set_uniform_int(sss_vertical_shader_, "uDepthTex", TextureSlots::kTAAHistory);
    ctx->push_command([h = sss_vertical_shader_, w = static_cast<float>(width), hh = static_cast<float>(height)](IRenderBackend* backend) {
        IShader* s = backend->shader(h);
        if (s) s->set_vec2("uScreenSize", math::Vector2f(w, hh));
    });
    ctx->set_uniform_float(sss_vertical_shader_, "uSSSStrength", params.sss_strength);
    ctx->set_uniform_float(sss_vertical_shader_, "uSSSScale", params.sss_scale);
    ctx->draw_mesh(fullscreen_mesh_, sss_vertical_shader_);

    // Pass 3: 合成到输出 FBO
    // 从 scene_color_tex（原始场景）和 sss_tex_[1]（模糊结果）混合
    ctx->set_framebuffer(output_fbo);
    ctx->set_viewport(0, 0, sss_w_, sss_h_);
    ctx->set_blend(false);
    ctx->set_texture(sss_composite_shader_, scene_color_tex, TextureSlots::kTonemapHDR, "uSceneColor");
    ctx->set_uniform_int(sss_composite_shader_, "uSceneColor", TextureSlots::kTonemapHDR);
    ctx->set_texture(sss_composite_shader_, sss_tex_[1], TextureSlots::kTAAHistory, "uSSSBlur");
    ctx->set_uniform_int(sss_composite_shader_, "uSSSBlur", TextureSlots::kTAAHistory);
    ctx->set_uniform_float(sss_composite_shader_, "uSSSStrength", params.sss_strength);
    ctx->draw_mesh(fullscreen_mesh_, sss_composite_shader_);
}

} // namespace gryce_engine::render