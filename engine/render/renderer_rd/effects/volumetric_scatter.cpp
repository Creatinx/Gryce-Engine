// volumetric_scatter.cpp — 体积光散射（丁达尔）的 RD 实现：目标管理与单次全屏
// 光线步进 pass 的调度。算法细节在 volumetric_scatter.frag / 其 vulkan 变体里。
#include "render/renderer_rd/effects/volumetric_scatter.h"

#include <algorithm>

#include "render/render_context.h"
#include "render/texture.h"
#include "render/framebuffer.h"
#include "render/mesh.h"
#include "render/shader.h"
#include "render/gpu_scope.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::render {

bool VolumetricScatter_RD::init(RenderContext* ctx, const std::string& shader_dir) {
    if (initialized_) return true;
    if (!ctx) return false;
    ctx_ = ctx;

    shader_ = ctx->create_shader();
    if (shader_.is_valid()) {
        IShader* s = ctx->shader(shader_);
        if (s && !s->load_program("volumetric_scatter", shader_dir, nullptr, true, true)) {
            GLOG_WARN("VolumetricScatter_RD: failed to load shader 'volumetric_scatter'");
        }
    }

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

void VolumetricScatter_RD::destroy() {
    if (!ctx_) return;
    destroy_targets();
    if (shader_.is_valid()) { ctx_->destroy_shader(shader_); shader_ = {}; }
    if (fullscreen_mesh_.is_valid()) { ctx_->destroy_mesh(fullscreen_mesh_); fullscreen_mesh_ = {}; }
    initialized_ = false;
}

bool VolumetricScatter_RD::create_targets(int width, int height) {
    destroy_targets();
    if (!ctx_) return false;

    const int w = std::max(16, width);
    const int h = std::max(16, height);

    out_tex_ = ctx_->create_texture();
    ITexture* tex = ctx_->texture(out_tex_);
    if (!out_tex_.is_valid() || !tex || !tex->create(TextureFormat::RGBA16F, w, h, nullptr)) {
        GLOG_ERROR("VolumetricScatter_RD: output texture failed ({}x{})", w, h);
        destroy_targets();
        return false;
    }
    tex->set_filter(TextureFilter::Linear, TextureFilter::Linear);
    tex->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

    out_fbo_ = ctx_->create_framebuffer();
    IFramebuffer* fbo = ctx_->framebuffer(out_fbo_);
    if (!out_fbo_.is_valid() || !fbo || !fbo->create(w, h)) {
        GLOG_ERROR("VolumetricScatter_RD: output framebuffer failed ({}x{})", w, h);
        destroy_targets();
        return false;
    }
    fbo->attach_color_texture(tex);
    if (!fbo->is_complete()) {
        GLOG_ERROR("VolumetricScatter_RD: output framebuffer incomplete ({}x{})", w, h);
        destroy_targets();
        return false;
    }

    out_w_ = w;
    out_h_ = h;
    return true;
}

void VolumetricScatter_RD::destroy_targets() {
    if (!ctx_) return;
    if (out_fbo_.is_valid()) { ctx_->destroy_framebuffer(out_fbo_); out_fbo_ = {}; }
    if (out_tex_.is_valid()) { ctx_->destroy_texture(out_tex_); out_tex_ = {}; }
    out_w_ = 0;
    out_h_ = 0;
}

void VolumetricScatter_RD::render(RenderContext* ctx,
                                  RHITextureHandle color_tex,
                                  RHITextureHandle depth_tex,
                                  RHITextureHandle shadow_map,
                                  const math::Matrix4f& view_proj,
                                  const math::Matrix4f& light_space,
                                  const math::Vector3f& camera_pos,
                                  const math::Vector3f& sun_direction,
                                  const math::Vector3f& sun_color,
                                  float sun_intensity,
                                  float ndc_z_scale, float ndc_z_bias,
                                  const PostProcessParams& params,
                                  int viewport_w, int viewport_h) {
    if (!ctx || !initialized_ || !shader_.is_valid() || !fullscreen_mesh_.is_valid()) return;
    if (!color_tex.is_valid() || !depth_tex.is_valid()) return;
    if (params.volumetric_enabled == 0) return;
    if (params.volumetric_steps <= 0 || params.volumetric_density <= 0.0f) return;
    // 视口尺寸变化（或首次）时重建输出目标
    if (out_w_ != viewport_w || out_h_ != viewport_h || !out_fbo_.is_valid()) {
        if (!create_targets(viewport_w, viewport_h)) return;
    }
    GpuScope _gpu(*ctx, "volumetric");

    const math::Matrix4f inv_view_proj = view_proj.inverse();

    // 目标必须显式设置：上一个 pass 的 FBO/视口沿用会把结果画进别的目标。
    ctx->set_framebuffer(out_fbo_);
    ctx->set_viewport(0, 0, out_w_, out_h_);
    ctx->set_shader(shader_);
    ctx->set_depth_test(false);
    ctx->set_depth_write(false);
    ctx->set_cull_face(CullMode::None);
    ctx->set_blend(false);

    ctx->set_texture(shader_, color_tex, TextureSlots::kTonemapHDR, "uSceneTex");
    ctx->set_uniform_int(shader_, "uSceneTex", TextureSlots::kTonemapHDR);
    // 深度按原始值采样（比较采样器读到的是比较结果，不是深度值）。
    ctx->set_texture_raw_depth(shader_, depth_tex, TextureSlots::kPBRShadowDepth, "uDepthTex");
    ctx->set_uniform_int(shader_, "uDepthTex", TextureSlots::kPBRShadowDepth);

    ctx->set_uniform_mat4(shader_, "uInvViewProj", inv_view_proj);
    // 日照遮挡（光柱来源）：有阴影贴图才开，否则显式关闭退化成均匀薄雾。
    const bool shadow_ok = shadow_map.is_valid();
    if (shadow_ok) {
        ctx->set_texture_raw_depth(shader_, shadow_map, TextureSlots::kPBRShadow, "uShadowMap");
        ctx->set_uniform_int(shader_, "uShadowMap", TextureSlots::kPBRShadow);
        ctx->set_uniform_mat4(shader_, "uLightSpaceMatrix", light_space);
    } else {
        ctx->set_texture_raw_depth(shader_, depth_tex, TextureSlots::kPBRShadow, "uShadowMap");
        ctx->set_uniform_int(shader_, "uShadowMap", TextureSlots::kPBRShadow);
        ctx->set_uniform_mat4(shader_, "uLightSpaceMatrix", math::Matrix4f::identity());
    }
    ctx->set_uniform_int(shader_, "uShadowEnabled", shadow_ok ? 1 : 0);
    ctx->set_uniform_float(shader_, "uShadowBias", 0.0025f);
    ctx->set_uniform_vec3(shader_, "uCameraPos", camera_pos);
    ctx->set_uniform_vec3(shader_, "uSunDirection", sun_direction);
    ctx->set_uniform_vec3(shader_, "uSunColor", sun_color);
    ctx->set_uniform_float(shader_, "uSunIntensity", sun_intensity);
    ctx->set_uniform_vec3(shader_, "uTint", params.volumetric_tint);
    ctx->set_uniform_float(shader_, "uIntensity", params.volumetric_intensity);
    ctx->set_uniform_float(shader_, "uDensity", params.volumetric_density);
    ctx->set_uniform_float(shader_, "uScattering", params.volumetric_scattering);
    ctx->set_uniform_float(shader_, "uPhaseG", params.volumetric_phase_g);
    ctx->set_uniform_int(shader_, "uSteps", std::max(0, std::min(64, params.volumetric_steps)));
    ctx->set_uniform_float(shader_, "uMaxDistance", params.volumetric_max_distance);
    ctx->set_uniform_float(shader_, "uNDCZScale", ndc_z_scale);
    ctx->set_uniform_float(shader_, "uNDCZBias", ndc_z_bias);

    ctx->draw_mesh(fullscreen_mesh_, shader_);

    ctx->set_depth_test(true);
    ctx->set_depth_write(true);
}

} // namespace gryce_engine::render
