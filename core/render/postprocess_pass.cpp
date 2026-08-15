#include "render_pipeline.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

#include "render/render_context.h"
#include "render/shader.h"
#include "render/texture.h"
#include "render/framebuffer.h"
#include "render/mesh.h"
#include "render/material.h"
#include "render/ibl_generator.h"
#include "assets/asset_manager.h"
#include "assets/texture_data.h"
#include "scene/scene.h"
#include "scene/entity.h"
#include "components/transform.h"
#include "components/mesh_renderer.h"
#include "components/skinned_mesh_renderer.h"
#include "scene/query.h"
#include "math/camera.h"
#include "resources/resource_path.h"
#include "utils/glog/glog_lib.h"
#include "audio/audio_engine.h"

namespace gryce_engine::render {

void RenderPipeline::set_contact_shadow_params(float strength, float radius_world, int steps) {
    contact_shadow_strength_ = math::clamp(strength, 0.0f, 2.0f);
    contact_shadow_radius_ = std::max(0.01f, radius_world);
    contact_shadow_steps_ = std::clamp(steps, 1, 32);
}


bool RenderPipeline::create_hdr_target(RenderContext* ctx) {
    hdr_color_ = ctx->create_texture();
    ITexture* hdr_color_ptr = ctx->texture(hdr_color_);
    if (!hdr_color_.is_valid() || !hdr_color_ptr || !hdr_color_ptr->create(TextureFormat::RGBA16F, viewport_width_, viewport_height_, nullptr)) {
        return false;
    }
    hdr_color_ptr->set_filter(TextureFilter::Linear, TextureFilter::Linear);
    hdr_color_ptr->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

    hdr_depth_ = ctx->create_texture();
    ITexture* hdr_depth_ptr = ctx->texture(hdr_depth_);
    if (!hdr_depth_.is_valid() || !hdr_depth_ptr || !hdr_depth_ptr->create(TextureFormat::Depth24, viewport_width_, viewport_height_, nullptr)) {
        return false;
    }

    hdr_fbo_ = ctx->create_framebuffer();
    IFramebuffer* hdr_fbo_ptr = ctx->framebuffer(hdr_fbo_);
    if (!hdr_fbo_.is_valid() || !hdr_fbo_ptr || !hdr_fbo_ptr->create(viewport_width_, viewport_height_)) {
        return false;
    }
    hdr_fbo_ptr->attach_color_texture(hdr_color_ptr);
    hdr_fbo_ptr->attach_depth_texture(hdr_depth_ptr);
    if (!hdr_fbo_ptr->is_complete()) {
        GLOG_ERROR("RenderPipeline: HDR framebuffer incomplete");
        return false;
    }
    return true;
}


bool RenderPipeline::create_bloom_targets(RenderContext* ctx) {
    // 半分辨率链：L0 = w/2 ... L4 = w/32（最小 16）
    int w = std::max(16, viewport_width_ / 2);
    int h = std::max(16, viewport_height_ / 2);
    for (int i = 0; i < k_bloom_levels; ++i) {
        bloom_level_w_[i] = w;
        bloom_level_h_[i] = h;

        bloom_down_tex_[i] = ctx->create_texture();
        ITexture* down_tex = ctx->texture(bloom_down_tex_[i]);
        if (!bloom_down_tex_[i].is_valid() || !down_tex ||
            !down_tex->create(TextureFormat::RGBA16F, w, h, nullptr)) {
            GLOG_ERROR("RenderPipeline: bloom down texture {} failed ({}x{})", i, w, h);
            return false;
        }
        down_tex->set_filter(TextureFilter::Linear, TextureFilter::Linear);
        down_tex->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

        bloom_down_fbo_[i] = ctx->create_framebuffer();
        IFramebuffer* down_fbo = ctx->framebuffer(bloom_down_fbo_[i]);
        if (!bloom_down_fbo_[i].is_valid() || !down_fbo || !down_fbo->create(w, h)) {
            return false;
        }
        down_fbo->attach_color_texture(down_tex);
        if (!down_fbo->is_complete()) return false;

        bloom_up_tex_[i] = ctx->create_texture();
        ITexture* up_tex = ctx->texture(bloom_up_tex_[i]);
        if (!bloom_up_tex_[i].is_valid() || !up_tex ||
            !up_tex->create(TextureFormat::RGBA16F, w, h, nullptr)) {
            GLOG_ERROR("RenderPipeline: bloom up texture {} failed ({}x{})", i, w, h);
            return false;
        }
        up_tex->set_filter(TextureFilter::Linear, TextureFilter::Linear);
        up_tex->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

        bloom_up_fbo_[i] = ctx->create_framebuffer();
        IFramebuffer* up_fbo = ctx->framebuffer(bloom_up_fbo_[i]);
        if (!bloom_up_fbo_[i].is_valid() || !up_fbo || !up_fbo->create(w, h)) {
            return false;
        }
        up_fbo->attach_color_texture(up_tex);
        if (!up_fbo->is_complete()) return false;

        w = std::max(16, w / 2);
        h = std::max(16, h / 2);
    }
    bloom_targets_valid_ = true;
    return true;
}


void RenderPipeline::destroy_bloom_targets() {
    if (!ctx_) return;
    for (auto& fbo : bloom_down_fbo_) {
        if (fbo.is_valid()) {
            ctx_->destroy_framebuffer(fbo);
            fbo = RHIFramebufferHandle{};
        }
    }
    for (auto& fbo : bloom_up_fbo_) {
        if (fbo.is_valid()) {
            ctx_->destroy_framebuffer(fbo);
            fbo = RHIFramebufferHandle{};
        }
    }
    for (auto& tex : bloom_down_tex_) {
        if (tex.is_valid()) {
            ctx_->destroy_texture(tex);
            tex = RHITextureHandle{};
        }
    }
    for (auto& tex : bloom_up_tex_) {
        if (tex.is_valid()) {
            ctx_->destroy_texture(tex);
            tex = RHITextureHandle{};
        }
    }
    bloom_targets_valid_ = false;
}


bool RenderPipeline::create_auto_exposure_targets(RenderContext* ctx) {
    // 亮度链：L0 = w/2 ... L4 = w/32，L5 = 1x1
    int w = std::max(16, viewport_width_ / 2);
    int h = std::max(16, viewport_height_ / 2);
    for (int i = 0; i < k_lum_levels; ++i) {
        const int tw = (i == k_lum_levels - 1) ? 1 : w;
        const int th = (i == k_lum_levels - 1) ? 1 : h;
        lum_w_[i] = tw;
        lum_h_[i] = th;

        lum_tex_[i] = ctx->create_texture();
        ITexture* tex = ctx->texture(lum_tex_[i]);
        if (!lum_tex_[i].is_valid() || !tex ||
            !tex->create(TextureFormat::RGBA16F, tw, th, nullptr)) {
            GLOG_ERROR("RenderPipeline: auto-exposure lum texture {} failed", i);
            return false;
        }
        tex->set_filter(TextureFilter::Linear, TextureFilter::Linear);
        tex->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
        lum_fbo_[i] = ctx->create_framebuffer();
        IFramebuffer* fbo = ctx->framebuffer(lum_fbo_[i]);
        if (!lum_fbo_[i].is_valid() || !fbo || !fbo->create(tw, th)) return false;
        fbo->attach_color_texture(tex);
        if (!fbo->is_complete()) return false;

        w = std::max(2, w / 2);
        h = std::max(2, h / 2);
    }

    // 双缓冲曝光值（1x1 RGBA16F），初始化为 1.0 避免首帧全黑
    const float one[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    for (int i = 0; i < 2; ++i) {
        exposure_tex_[i] = ctx->create_texture();
        ITexture* tex = ctx->texture(exposure_tex_[i]);
        if (!exposure_tex_[i].is_valid() || !tex ||
            !tex->create(TextureFormat::RGBA16F, 1, 1, one)) {
            return false;
        }
        tex->set_filter(TextureFilter::Linear, TextureFilter::Linear);
        tex->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
        exposure_fbo_[i] = ctx->create_framebuffer();
        IFramebuffer* fbo = ctx->framebuffer(exposure_fbo_[i]);
        if (!exposure_fbo_[i].is_valid() || !fbo || !fbo->create(1, 1)) return false;
        fbo->attach_color_texture(tex);
        if (!fbo->is_complete()) return false;
    }
    auto_exposure_targets_valid_ = true;
    return true;
}


void RenderPipeline::destroy_auto_exposure_targets() {
    if (!ctx_) return;
    for (auto& fbo : lum_fbo_) {
        if (fbo.is_valid()) { ctx_->destroy_framebuffer(fbo); fbo = RHIFramebufferHandle{}; }
    }
    for (auto& tex : lum_tex_) {
        if (tex.is_valid()) { ctx_->destroy_texture(tex); tex = RHITextureHandle{}; }
    }
    for (auto& fbo : exposure_fbo_) {
        if (fbo.is_valid()) { ctx_->destroy_framebuffer(fbo); fbo = RHIFramebufferHandle{}; }
    }
    for (auto& tex : exposure_tex_) {
        if (tex.is_valid()) { ctx_->destroy_texture(tex); tex = RHITextureHandle{}; }
    }
    auto_exposure_targets_valid_ = false;
}


bool RenderPipeline::create_taa_targets(RenderContext* ctx) {
    for (int i = 0; i < 2; ++i) {
        taa_tex_[i] = ctx->create_texture();
        ITexture* tex = ctx->texture(taa_tex_[i]);
        if (!taa_tex_[i].is_valid() || !tex ||
            !tex->create(TextureFormat::RGBA16F, viewport_width_, viewport_height_, nullptr)) {
            return false;
        }
        tex->set_filter(TextureFilter::Linear, TextureFilter::Linear);
        tex->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
        taa_fbo_[i] = ctx->create_framebuffer();
        IFramebuffer* fbo = ctx->framebuffer(taa_fbo_[i]);
        if (!taa_fbo_[i].is_valid() || !fbo ||
            !fbo->create(viewport_width_, viewport_height_)) {
            return false;
        }
        fbo->attach_color_texture(tex);
        if (!fbo->is_complete()) return false;
    }
    taa_targets_valid_ = true;
    return true;
}


void RenderPipeline::destroy_taa_targets() {
    if (!ctx_) return;
    for (auto& fbo : taa_fbo_) {
        if (fbo.is_valid()) { ctx_->destroy_framebuffer(fbo); fbo = RHIFramebufferHandle{}; }
    }
    for (auto& tex : taa_tex_) {
        if (tex.is_valid()) { ctx_->destroy_texture(tex); tex = RHITextureHandle{}; }
    }
    taa_targets_valid_ = false;
}


float RenderPipeline::halton(uint32_t index, uint32_t base) {
    float f = 1.0f;
    float r = 0.0f;
    while (index > 0) {
        f /= static_cast<float>(base);
        r += f * static_cast<float>(index % base);
        index /= base;
    }
    return r;
}


bool RenderPipeline::create_ssao_targets(RenderContext* ctx) {
    ssao_w_ = std::max(16, viewport_width_ / 2);
    ssao_h_ = std::max(16, viewport_height_ / 2);
    // 1x1 白回退纹理：SSAO 禁用时仍绑定合法纹理（Vulkan 需要 SHADER_READ_ONLY layout）
    if (!ssao_fallback_tex_.is_valid()) {
        ssao_fallback_tex_ = ctx->create_texture();
        ITexture* ft = ctx->texture(ssao_fallback_tex_);
        if (!ssao_fallback_tex_.is_valid() || !ft) return false;
        const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        if (!ft->create(TextureFormat::RGBA16F, 1, 1, white)) return false;
        ft->set_filter(TextureFilter::Linear, TextureFilter::Linear);
        ft->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
    }
    for (int i = 0; i < 2; ++i) {
        ssao_tex_[i] = ctx->create_texture();
        ITexture* tex = ctx->texture(ssao_tex_[i]);
        if (!ssao_tex_[i].is_valid() || !tex ||
            !tex->create(TextureFormat::RGBA16F, ssao_w_, ssao_h_, nullptr)) {
            return false;
        }
        tex->set_filter(TextureFilter::Linear, TextureFilter::Linear);
        tex->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
        ssao_fbo_[i] = ctx->create_framebuffer();
        IFramebuffer* fbo = ctx->framebuffer(ssao_fbo_[i]);
        if (!ssao_fbo_[i].is_valid() || !fbo || !fbo->create(ssao_w_, ssao_h_)) return false;
        fbo->attach_color_texture(tex);
        if (!fbo->is_complete()) return false;
    }
    ssao_targets_valid_ = true;
    return true;
}


void RenderPipeline::destroy_ssao_targets() {
    if (!ctx_) return;
    if (ssao_fallback_tex_.is_valid()) {
        ctx_->destroy_texture(ssao_fallback_tex_);
        ssao_fallback_tex_ = RHITextureHandle{};
    }
    for (auto& fbo : ssao_fbo_) {
        if (fbo.is_valid()) { ctx_->destroy_framebuffer(fbo); fbo = RHIFramebufferHandle{}; }
    }
    for (auto& tex : ssao_tex_) {
        if (tex.is_valid()) { ctx_->destroy_texture(tex); tex = RHITextureHandle{}; }
    }
    ssao_targets_valid_ = false;
}


bool RenderPipeline::create_contact_shadow_targets(RenderContext* ctx) {
    cs_w_ = std::max(16, viewport_width_ / 2);
    cs_h_ = std::max(16, viewport_height_ / 2);
    contact_shadow_tex_ = ctx->create_texture();
    ITexture* tex = ctx->texture(contact_shadow_tex_);
    if (!contact_shadow_tex_.is_valid() || !tex ||
        !tex->create(TextureFormat::RGBA16F, cs_w_, cs_h_, nullptr)) {
        return false;
    }
    tex->set_filter(TextureFilter::Linear, TextureFilter::Linear);
    tex->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
    contact_shadow_fbo_ = ctx->create_framebuffer();
    IFramebuffer* fbo = ctx->framebuffer(contact_shadow_fbo_);
    if (!contact_shadow_fbo_.is_valid() || !fbo || !fbo->create(cs_w_, cs_h_)) return false;
    fbo->attach_color_texture(tex);
    if (!fbo->is_complete()) return false;
    contact_shadow_targets_valid_ = true;
    return true;
}


void RenderPipeline::destroy_contact_shadow_targets() {
    if (!ctx_) return;
    if (contact_shadow_fbo_.is_valid()) {
        ctx_->destroy_framebuffer(contact_shadow_fbo_);
        contact_shadow_fbo_ = RHIFramebufferHandle{};
    }
    if (contact_shadow_tex_.is_valid()) {
        ctx_->destroy_texture(contact_shadow_tex_);
        contact_shadow_tex_ = RHITextureHandle{};
    }
    contact_shadow_targets_valid_ = false;
}


void RenderPipeline::render_ssao(RenderContext& ctx) {
    if (pp_params_.ssao_enabled == 0 || !ssao_targets_valid_) return;
    if (!gtao_shader_.is_valid() || !ssao_blur_shader_.is_valid() || !fullscreen_mesh_.is_valid()) {
        return;
    }

    // 每帧从相机更新 GTAO 参数（Vulkan push constants / GL bind 时应用）
    if (camera_) {
        pp_params_.ssao_near = camera_->near_plane();
        pp_params_.ssao_far = camera_->far_plane();
        pp_params_.ssao_tan_half = std::tan(math::to_radians(camera_->fov()) * 0.5f);
        pp_params_.ssao_aspect = camera_->aspect();
    }
    for (RHIShaderHandle h : {gtao_shader_, ssao_blur_shader_}) {
        IShader* s = ctx_->shader(h);
        if (s) s->set_post_process_params(pp_params_);
    }

    ctx.set_depth_test(false);
    ctx.set_cull_face(false);
    ctx.set_blend(false);

    // Pass 1：GTAO（从深度重建视图位置，地平线搜索）
    ctx.set_framebuffer(ssao_fbo_[0]);
    ctx.set_viewport(0, 0, ssao_w_, ssao_h_);
    ctx.set_shader(gtao_shader_);
    ctx.set_texture_raw_depth(gtao_shader_, hdr_depth_, TextureSlots::kTonemapHDR, "uDepthTexture");
    ctx.set_uniform_int(gtao_shader_, "uDepthTexture", TextureSlots::kTonemapHDR);
    ctx.draw_mesh(fullscreen_mesh_, gtao_shader_);

    // Pass 2：深度感知双边上模糊
    ctx.set_framebuffer(ssao_fbo_[1]);
    ctx.set_viewport(0, 0, ssao_w_, ssao_h_);
    ctx.set_shader(ssao_blur_shader_);
    ITexture* ao = ctx_->texture(ssao_tex_[0]);
    if (ao) ao->bind(TextureSlots::kTonemapHDR);
    ctx.set_texture(ssao_blur_shader_, ssao_tex_[0], TextureSlots::kTonemapHDR, "uTexture");
    ctx.set_uniform_int(ssao_blur_shader_, "uTexture", TextureSlots::kTonemapHDR);
    ctx.set_texture_raw_depth(ssao_blur_shader_, hdr_depth_, TextureSlots::kTAAHistory, "uDepthTexture");
    ctx.set_uniform_int(ssao_blur_shader_, "uDepthTexture", TextureSlots::kTAAHistory);
    ctx.draw_mesh(fullscreen_mesh_, ssao_blur_shader_);
}


void RenderPipeline::render_contact_shadow(RenderContext& ctx) {
    if (!contact_shadow_enabled_ || !contact_shadow_targets_valid_) return;
    if (!contact_shadow_shader_.is_valid() || !fullscreen_mesh_.is_valid()) return;
    if (!camera_) return;

    const float near_p = camera_->near_plane();
    const float far_p = camera_->far_plane();
    const float tan_half = std::tan(math::to_radians(camera_->fov()) * 0.5f);
    const float aspect = camera_->aspect();

    // 取第一个方向光，步进方向指向光源（-direction）
    math::Vector3f light_dir = math::Vector3f(0.0f, -1.0f, 0.0f);
    for (const auto& l : lights_) {
        if (l.type == LightType::Directional) { light_dir = l.direction; break; }
    }
    const math::Matrix4f view = camera_->get_view_matrix();
    const math::Vector3f light_dir_view = view.transform_vector(math::Vector3f(-light_dir.x, -light_dir.y, -light_dir.z));

    ctx.set_depth_test(false);
    ctx.set_cull_face(false);
    ctx.set_blend(false);

    ctx.set_framebuffer(contact_shadow_fbo_);
    ctx.set_viewport(0, 0, cs_w_, cs_h_);
    ctx.set_shader(contact_shadow_shader_);
    ctx.set_texture_raw_depth(contact_shadow_shader_, hdr_depth_, TextureSlots::kTonemapHDR, "uDepthTexture");
    ctx.set_uniform_int(contact_shadow_shader_, "uDepthTexture", TextureSlots::kTonemapHDR);
    ctx.set_uniform_float(contact_shadow_shader_, "uCSNear", near_p);
    ctx.set_uniform_float(contact_shadow_shader_, "uCSFar", far_p);
    ctx.set_uniform_float(contact_shadow_shader_, "uCSTanHalfFov", tan_half);
    ctx.set_uniform_float(contact_shadow_shader_, "uCSAspect", aspect);
    ctx.set_uniform_vec3(contact_shadow_shader_, "uCSLightDirView", light_dir_view);
    ctx.set_uniform_float(contact_shadow_shader_, "uCSRadius", contact_shadow_radius_);
    ctx.set_uniform_int(contact_shadow_shader_, "uCSteps", contact_shadow_steps_);
    ctx.set_uniform_float(contact_shadow_shader_, "uCSStrength", contact_shadow_strength_);
    ctx.set_uniform_int(contact_shadow_shader_, "uCSEnabled", 1);
    ctx.draw_mesh(fullscreen_mesh_, contact_shadow_shader_);
}


bool RenderPipeline::create_viewport_target(RenderContext* ctx) {
    // tonemap 输出为 LDR，RGBA8 足够
    viewport_color_ = ctx->create_texture();
    ITexture* color_ptr = ctx->texture(viewport_color_);
    if (!viewport_color_.is_valid() || !color_ptr ||
        !color_ptr->create(TextureFormat::RGBA8, viewport_width_, viewport_height_, nullptr)) {
        return false;
    }
    color_ptr->set_filter(TextureFilter::Linear, TextureFilter::Linear);
    color_ptr->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

    viewport_fbo_ = ctx->create_framebuffer();
    IFramebuffer* fbo_ptr = ctx->framebuffer(viewport_fbo_);
    if (!viewport_fbo_.is_valid() || !fbo_ptr ||
        !fbo_ptr->create(viewport_width_, viewport_height_)) {
        return false;
    }
    fbo_ptr->attach_color_texture(color_ptr);
    if (!fbo_ptr->is_complete()) {
        GLOG_ERROR("RenderPipeline: viewport framebuffer incomplete");
        return false;
    }
    return true;
}


ITexture* RenderPipeline::viewport_color_texture() const {
    if (!ctx_ || !viewport_color_.is_valid()) return nullptr;
    return ctx_->texture(viewport_color_);
}


void RenderPipeline::render_tonemap(RenderContext& ctx) {
    if (!tonemap_shader_.is_valid() || !hdr_color_.is_valid() || !fullscreen_mesh_.is_valid()) {
        GLOG_WARN("RenderPipeline::render_tonemap: skipped tonemap_shader={} hdr_color={} fullscreen_mesh={}",
                  tonemap_shader_.is_valid(), hdr_color_.is_valid(), fullscreen_mesh_.is_valid());
        return;
    }

    // TAA 开启时读取 TAA 解析后的 HDR，否则读原始 HDR
    const bool use_taa = pp_params_.taa_enabled != 0 && taa_targets_valid_;
    const RHITextureHandle hdr_in = use_taa ? taa_tex_[taa_ping_] : hdr_color_;

    // 编辑器视口输出开启时，tonemap 写入独立 FBO 供 Viewport 面板采样，
    // 默认 framebuffer 只用于 ImGui；否则按原路径直接输出到屏幕。
    const bool to_viewport = viewport_output_enabled_ && viewport_fbo_.is_valid();
    GLOG_DEBUG("RenderPipeline::render_tonemap: to_viewport={} viewport_fbo_={} viewport={}x{}",
              to_viewport, viewport_fbo_.index, viewport_width_, viewport_height_);
    ctx.set_framebuffer(to_viewport ? viewport_fbo_ : RHIFramebufferHandle{});
    ctx.set_viewport(0, 0, viewport_width_, viewport_height_);
    ctx.set_depth_test(false);
    ctx.set_cull_face(false);
    ctx.set_blend(false);

    ctx.set_shader(tonemap_shader_);

    ITexture* hdr_color_ptr = ctx_->texture(hdr_in);
    IShader* tonemap_ptr = ctx_->shader(tonemap_shader_);
    IMesh* fullscreen_ptr = ctx_->mesh(fullscreen_mesh_);
    if (!hdr_color_ptr || !tonemap_ptr || !fullscreen_ptr) return;

    hdr_color_ptr->bind(TextureSlots::kTonemapHDR);
    ctx.set_texture(tonemap_shader_, hdr_in, TextureSlots::kTonemapHDR, "");
    ctx.set_uniform_int(tonemap_shader_, "uHDRTexture", TextureSlots::kTonemapHDR);
    // Bloom 输入：始终绑定合法纹理（禁用时绑 HDR 自身，shader 以 uBloomEnabled 跳过）
    const RHITextureHandle bloom_tex =
        (pp_params_.bloom_enabled != 0 && bloom_targets_valid_) ? bloom_up_tex_[0] : hdr_in;
    ITexture* bloom_ptr = ctx_->texture(bloom_tex);
    if (bloom_ptr) bloom_ptr->bind(TextureSlots::kTonemapBloom);
    ctx.set_texture(tonemap_shader_, bloom_tex, TextureSlots::kTonemapBloom, "uBloomTexture");
    ctx.set_uniform_int(tonemap_shader_, "uBloomTexture", TextureSlots::kTonemapBloom);
    // 3D LUT
    // 始终绑定合法纹理（未设置 LUT 时回退 HDR 自身，shader 以 uUseLUT 跳过）
    const RHITextureHandle lut_in = lut_texture_.is_valid() ? lut_texture_ : hdr_in;
    ITexture* lut = ctx_->texture(lut_in);
    if (lut) lut->bind(TextureSlots::kTonemapLUT);
    ctx.set_texture(tonemap_shader_, lut_in, TextureSlots::kTonemapLUT, "uLUTTexture");
    ctx.set_uniform_int(tonemap_shader_, "uLUTTexture", TextureSlots::kTonemapLUT);
    // 自动曝光值（1x1）
    const RHITextureHandle exp_in =
        (exposure_tex_[current_exposure_idx_].is_valid() && auto_exposure_targets_valid_)
            ? exposure_tex_[current_exposure_idx_]
            : hdr_in;
    ITexture* exp = ctx_->texture(exp_in);
    if (exp) exp->bind(TextureSlots::kTonemapExposure);
    ctx.set_texture(tonemap_shader_, exp_in, TextureSlots::kTonemapExposure, "uExposureTexture");
    ctx.set_uniform_int(tonemap_shader_, "uExposureTexture", TextureSlots::kTonemapExposure);
    // 屏幕空间接触阴影：半分辨率因子贴图，乘到 HDR 颜色（禁用时保持全亮）
    if (contact_shadow_enabled_ && contact_shadow_targets_valid_ && contact_shadow_tex_.is_valid()) {
        ctx.set_texture(tonemap_shader_, contact_shadow_tex_, TextureSlots::kTonemapContactShadow, "uContactShadowTexture");
        ctx.set_uniform_int(tonemap_shader_, "uContactShadowTexture", TextureSlots::kTonemapContactShadow);
        ctx.set_uniform_int(tonemap_shader_, "uContactShadowEnabled", 1);
        ctx.set_uniform_float(tonemap_shader_, "uContactShadowStrength", contact_shadow_strength_);
    } else {
        ctx.set_uniform_int(tonemap_shader_, "uContactShadowEnabled", 0);
    }
    tonemap_ptr->set_post_process_params(pp_params_);

    ctx.draw_mesh(fullscreen_mesh_, tonemap_shader_);
}

// ---------------------------------------------------------------------------
// Bloom：阈值提取 → 多级降采样模糊 → 上采样合成（半分辨率）
// ---------------------------------------------------------------------------

void RenderPipeline::render_bloom(RenderContext& ctx) {
    if (!bloom_targets_valid_ || !fullscreen_mesh_.is_valid()) return;
    if (!bloom_threshold_shader_.is_valid() || !bloom_downsample_shader_.is_valid() ||
        !bloom_upsample_shader_.is_valid()) {
        return;
    }

    ctx.set_depth_test(false);
    ctx.set_cull_face(false);
    ctx.set_blend(false);

    // 后处理参数同步到 bloom shader（Vulkan push constants / GL bind 时应用）
    for (RHIShaderHandle h : {bloom_threshold_shader_, bloom_downsample_shader_, bloom_upsample_shader_}) {
        IShader* s = ctx_->shader(h);
        if (s) s->set_post_process_params(pp_params_);
    }

    // 1. Threshold：HDR 全分辨率 → D0（半分辨率）
    {
        ctx.set_framebuffer(bloom_down_fbo_[0]);
        ctx.set_viewport(0, 0, bloom_level_w_[0], bloom_level_h_[0]);
        ctx.set_shader(bloom_threshold_shader_);
        ITexture* hdr = ctx_->texture(hdr_color_);
        if (hdr) hdr->bind(TextureSlots::kTonemapHDR);
        ctx.set_texture(bloom_threshold_shader_, hdr_color_, TextureSlots::kTonemapHDR, "uTexture");
        ctx.set_uniform_int(bloom_threshold_shader_, "uTexture", TextureSlots::kTonemapHDR);
        ctx.set_uniform_float(bloom_threshold_shader_, "uBloomThreshold", pp_params_.bloom_threshold);
        ctx.draw_mesh(fullscreen_mesh_, bloom_threshold_shader_);
    }

    // 2. 降采样模糊链：D0→D1→D2→D3，D3→U4
    for (int i = 1; i < k_bloom_levels; ++i) {
        const bool last = (i == k_bloom_levels - 1);
        const RHIFramebufferHandle target = last ? bloom_up_fbo_[i] : bloom_down_fbo_[i];
        const RHITextureHandle src = bloom_down_tex_[i - 1];
        ctx.set_framebuffer(target);
        ctx.set_viewport(0, 0, bloom_level_w_[i], bloom_level_h_[i]);
        ctx.set_shader(bloom_downsample_shader_);
        ITexture* src_tex = ctx_->texture(src);
        if (src_tex) src_tex->bind(TextureSlots::kTonemapHDR);
        ctx.set_texture(bloom_downsample_shader_, src, TextureSlots::kTonemapHDR, "uTexture");
        ctx.set_uniform_int(bloom_downsample_shader_, "uTexture", TextureSlots::kTonemapHDR);
        ctx.draw_mesh(fullscreen_mesh_, bloom_downsample_shader_);
    }

    // 3. 上采样合成：U_i = blur(U_{i+1}) + D_i（从最小编往回）
    for (int i = k_bloom_levels - 2; i >= 0; --i) {
        ctx.set_framebuffer(bloom_up_fbo_[i]);
        ctx.set_viewport(0, 0, bloom_level_w_[i], bloom_level_h_[i]);
        ctx.set_shader(bloom_upsample_shader_);
        ITexture* a = ctx_->texture(bloom_up_tex_[i + 1]);
        if (a) a->bind(TextureSlots::kTonemapHDR);
        ctx.set_texture(bloom_upsample_shader_, bloom_up_tex_[i + 1],
                        TextureSlots::kTonemapHDR, "uTextureA");
        ctx.set_uniform_int(bloom_upsample_shader_, "uTextureA", TextureSlots::kTonemapHDR);
        ITexture* b = ctx_->texture(bloom_down_tex_[i]);
        if (b) b->bind(TextureSlots::kTonemapBloom);
        ctx.set_texture(bloom_upsample_shader_, bloom_down_tex_[i],
                        TextureSlots::kTonemapBloom, "uTextureB");
        ctx.set_uniform_int(bloom_upsample_shader_, "uTextureB", TextureSlots::kTonemapBloom);
        ctx.draw_mesh(fullscreen_mesh_, bloom_upsample_shader_);
    }
}

// ---------------------------------------------------------------------------
// 自动曝光：HDR → 亮度链（逐级 4-tap 平均）→ 1x1 → 曝光更新（双缓冲反馈）
// ---------------------------------------------------------------------------

void RenderPipeline::render_auto_exposure(RenderContext& ctx) {
    if (pp_params_.auto_exposure == 0 || !auto_exposure_targets_valid_) return;
    if (!lum_average_shader_.is_valid() || !exposure_update_shader_.is_valid() ||
        !fullscreen_mesh_.is_valid()) {
        return;
    }

    ctx.set_depth_test(false);
    ctx.set_cull_face(false);
    ctx.set_blend(false);
    for (RHIShaderHandle h : {lum_average_shader_, exposure_update_shader_}) {
        IShader* s = ctx_->shader(h);
        if (s) s->set_post_process_params(pp_params_);
    }

    // 亮度链：HDR → L0 ... L4 → 1x1
    RHITextureHandle src = hdr_color_;
    for (int i = 0; i < k_lum_levels; ++i) {
        ctx.set_framebuffer(lum_fbo_[i]);
        ctx.set_viewport(0, 0, lum_w_[i], lum_h_[i]);
        ctx.set_shader(lum_average_shader_);
        ITexture* tex = ctx_->texture(src);
        if (tex) tex->bind(TextureSlots::kTonemapHDR);
        ctx.set_texture(lum_average_shader_, src, TextureSlots::kTonemapHDR, "uTexture");
        ctx.set_uniform_int(lum_average_shader_, "uTexture", TextureSlots::kTonemapHDR);
        ctx.draw_mesh(fullscreen_mesh_, lum_average_shader_);
        src = lum_tex_[i];
    }

    // 曝光更新：新曝光 = lerp(旧曝光, clamp(target/avg, min, max), speed)
    const int write_idx = exposure_ping_;
    ctx.set_framebuffer(exposure_fbo_[write_idx]);
    ctx.set_viewport(0, 0, 1, 1);
    ctx.set_shader(exposure_update_shader_);
    ITexture* lum1x1 = ctx_->texture(lum_tex_[k_lum_levels - 1]);
    if (lum1x1) lum1x1->bind(TextureSlots::kTonemapHDR);
    ctx.set_texture(exposure_update_shader_, lum_tex_[k_lum_levels - 1],
                    TextureSlots::kTonemapHDR, "uTexture");
    ctx.set_uniform_int(exposure_update_shader_, "uTexture", TextureSlots::kTonemapHDR);
    ITexture* prev = ctx_->texture(exposure_tex_[1 - write_idx]);
    if (prev) prev->bind(TextureSlots::kTAAHistory);
    ctx.set_texture(exposure_update_shader_, exposure_tex_[1 - write_idx],
                    TextureSlots::kTAAHistory, "uPrevExposure");
    ctx.set_uniform_int(exposure_update_shader_, "uPrevExposure", TextureSlots::kTAAHistory);
    ctx.draw_mesh(fullscreen_mesh_, exposure_update_shader_);

    current_exposure_idx_ = write_idx;
    exposure_ping_ = 1 - write_idx;
}

// ---------------------------------------------------------------------------
// TAA v1：半像素抖动 + 历史帧 + 邻域钳制（无运动矢量重投影）
// ---------------------------------------------------------------------------

void RenderPipeline::render_taa(RenderContext& ctx) {
    if (pp_params_.taa_enabled == 0 || !taa_targets_valid_) return;
    if (!taa_resolve_shader_.is_valid() || !fullscreen_mesh_.is_valid()) return;

    const int read = 1 - taa_ping_;
    const int write = taa_ping_;

    ctx.set_depth_test(false);
    ctx.set_cull_face(false);
    ctx.set_blend(false);
    if (IShader* s = ctx_->shader(taa_resolve_shader_)) {
        s->set_post_process_params(pp_params_);
    }

    ctx.set_framebuffer(taa_fbo_[write]);
    ctx.set_viewport(0, 0, viewport_width_, viewport_height_);
    ctx.set_shader(taa_resolve_shader_);
    ITexture* cur = ctx_->texture(hdr_color_);
    if (cur) cur->bind(TextureSlots::kTonemapHDR);
    ctx.set_texture(taa_resolve_shader_, hdr_color_, TextureSlots::kTonemapHDR, "uHDRTexture");
    ctx.set_uniform_int(taa_resolve_shader_, "uHDRTexture", TextureSlots::kTonemapHDR);
    ITexture* hist = ctx_->texture(taa_tex_[read]);
    if (hist) hist->bind(TextureSlots::kTAAHistory);
    ctx.set_texture(taa_resolve_shader_, taa_tex_[read], TextureSlots::kTAAHistory, "uHistoryTexture");
    ctx.set_uniform_int(taa_resolve_shader_, "uHistoryTexture", TextureSlots::kTAAHistory);
    ctx.draw_mesh(fullscreen_mesh_, taa_resolve_shader_);

    taa_ping_ = write;
}


bool RenderPipeline::create_lut_texture(RenderContext* ctx, const assets::TextureData* data) {
    if (!ctx || !data) return false;
    lut_texture_ = ctx->create_texture();
    ITexture* tex = ctx->texture(lut_texture_);
    if (!lut_texture_.is_valid() || !tex) return false;
    if (!tex->upload_data(data->data(), data->width, data->height, data->channels)) {
        ctx->destroy_texture(lut_texture_);
        lut_texture_ = RHITextureHandle{};
        return false;
    }
    tex->set_filter(TextureFilter::Linear, TextureFilter::Linear);
    tex->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
    GLOG_INFO("RenderPipeline: color LUT loaded ({}x{}, {} channels)", data->width, data->height,
              data->channels);
    return true;
}


void RenderPipeline::set_color_lut(const std::string& path) {
    if (!ctx_) return;
    // 记录路径，供 hot_reload() 重建后恢复 LUT
    lut_path_ = path;
    lut_set_ = !path.empty();
    if (path.empty()) {
        auto destroy = [this] {
            if (lut_texture_.is_valid()) {
                ctx_->destroy_texture(lut_texture_);
                lut_texture_ = RHITextureHandle{};
            }
        };
        if (initialized_) {
            ctx_->push_command([destroy](render::IRenderBackend*) { destroy(); });
        } else {
            destroy();
        }
        return;
    }
    auto tex_data = assets::AssetManager::instance().load<assets::TextureData>(path);
    if (!tex_data.valid()) {
        GLOG_ERROR("RenderPipeline::set_color_lut: failed to load '{}'", path);
        return;
    }
    std::shared_ptr<const assets::TextureData> data = tex_data.shared();
    if (!initialized_) {
        if (lut_texture_.is_valid()) {
            ctx_->destroy_texture(lut_texture_);
            lut_texture_ = RHITextureHandle{};
        }
        create_lut_texture(ctx_, data.get());
        return;
    }
    // 运行时上传：渲染线程创建纹理（shared_ptr 按值捕获保证数据存活）
    ctx_->push_command([this, data](render::IRenderBackend*) {
        if (lut_texture_.is_valid()) {
            ctx_->destroy_texture(lut_texture_);
            lut_texture_ = RHITextureHandle{};
        }
        create_lut_texture(ctx_, data.get());
    });
}


void RenderPipeline::set_auto_exposure_params(float target_luminance, float min_exposure,
                                              float max_exposure, float speed) {
    pp_params_.ae_target_luminance = std::max(1e-4f, target_luminance);
    pp_params_.ae_min_exposure = std::max(1e-3f, min_exposure);
    pp_params_.ae_max_exposure = std::max(pp_params_.ae_min_exposure, max_exposure);
    pp_params_.ae_speed = math::clamp(speed, 0.01f, 1.0f);
}

// ---------------------------------------------------------------------------
// Scene View 网格线
// ---------------------------------------------------------------------------

} // namespace gryce_engine::render
