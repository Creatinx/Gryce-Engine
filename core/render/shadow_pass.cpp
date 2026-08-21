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

bool RenderPipeline::create_cascade_shadow_maps(RenderContext* ctx) {
    for (int i = 0; i < k_max_cascades; ++i) {
        const int size = std::max(64, cascade_sizes_[i]);

        shadow_maps_[i] = ctx->create_texture();
        ITexture* shadow_map_ptr = ctx->texture(shadow_maps_[i]);
        if (!shadow_maps_[i].is_valid() || !shadow_map_ptr ||
            !shadow_map_ptr->create_depth(size, size)) {
            GLOG_ERROR("RenderPipeline: failed to create cascade {} shadow texture", i);
            return false;
        }
        shadow_map_ptr->set_filter(TextureFilter::Linear, TextureFilter::Linear);
        shadow_map_ptr->set_wrap(TextureWrap::ClampToBorder, TextureWrap::ClampToBorder);

        shadow_fbos_[i] = ctx->create_framebuffer();
        IFramebuffer* shadow_fbo_ptr = ctx->framebuffer(shadow_fbos_[i]);
        if (!shadow_fbos_[i].is_valid() || !shadow_fbo_ptr ||
            !shadow_fbo_ptr->create(size, size)) {
            GLOG_ERROR("RenderPipeline: failed to create cascade {} shadow framebuffer", i);
            return false;
        }
        shadow_fbo_ptr->attach_depth_texture(shadow_map_ptr);
        if (!shadow_fbo_ptr->is_complete()) {
            GLOG_ERROR("RenderPipeline: cascade {} shadow framebuffer incomplete", i);
            return false;
        }
    }
    return true;
}


bool RenderPipeline::resize_shadow_map(RenderContext* ctx) {
    if (!ctx) return false;
    if (!initialized_) return true;

    for (auto& tex : shadow_maps_) {
        if (tex.is_valid()) {
            ctx->destroy_texture(tex);
            tex = RHITextureHandle{};
        }
    }
    for (auto& fb : shadow_fbos_) {
        if (fb.is_valid()) {
            ctx->destroy_framebuffer(fb);
            fb = RHIFramebufferHandle{};
        }
    }
    return create_cascade_shadow_maps(ctx);
}


void RenderPipeline::set_shadow_bias(float bias) {
    shadow_bias_ = bias;
    cascade_biases_.fill(bias);
}


void RenderPipeline::set_shadow_map_size(int size) {
    if (initialized_) return;
    cascade_sizes_.fill(std::max(64, size));
}


void RenderPipeline::set_cascade_count(int count) {
    cascade_count_ = math::clamp(count, 1, k_max_cascades);
}


void RenderPipeline::set_cascade_split_lambda(float lambda) {
    cascade_split_lambda_ = math::clamp(lambda, 0.0f, 1.0f);
}


void RenderPipeline::set_cascade_sizes(const std::array<int, k_max_cascades>& sizes) {
    if (initialized_) return;
    for (int i = 0; i < k_max_cascades; ++i) {
        cascade_sizes_[i] = std::max(64, sizes[i]);
    }
}


void RenderPipeline::set_cascade_biases(const std::array<float, k_max_cascades>& biases) {
    for (int i = 0; i < k_max_cascades; ++i) {
        cascade_biases_[i] = std::max(0.0f, biases[i]);
    }
}


void RenderPipeline::set_pcss_params(float light_size, float max_radius_texels, float tap_scale) {
    pcss_light_size_ = std::max(0.0f, light_size);
    pcss_max_radius_ = std::max(1.0f, max_radius_texels);
    pcss_tap_scale_ = std::max(0.1f, tap_scale);
}


ITexture* RenderPipeline::shadow_map() const {
    return ctx_ ? ctx_->texture(shadow_maps_[0]) : nullptr;
}


void RenderPipeline::update_light_space_matrix() {
    // 阴影只由第一个方向光投射；无相机/无方向光时级联矩阵保持单位阵。
    shadow_light_index_ = -1;
    for (size_t i = 0; i < lights_.size(); ++i) {
        if (lights_[i].type == LightType::Directional) {
            shadow_light_index_ = static_cast<int>(i);
            break;
        }
    }
    for (int c = 0; c < k_max_cascades; ++c) {
        cascade_light_space_matrices_[c] = math::Matrix4f::identity();
    }
    light_space_matrix_ = math::Matrix4f::identity();
    if (shadow_light_index_ < 0 || !camera_) return;

    // 方向光方向为零向量时 normalized() 产生 NaN，look_at 会崩坏；先判零再归一化。
    const math::Vector3f& raw_dir = lights_[shadow_light_index_].direction;
    math::Vector3f light_dir = raw_dir.length_sq() < 1e-6f
                               ? math::Vector3f(0.0f, -1.0f, 0.0f)
                               : raw_dir.normalized();

    // 当方向光与世界 up 平行时，look_at 的默认 up 会产生 gimbal lock；换一个 up。
    math::Vector3f up = math::Vector3f::up();
    if (std::abs(light_dir.dot(up)) > 0.98f) {
        up = math::Vector3f::forward();
    }

    const float cam_near = camera_->near_plane();
    const float cam_far = camera_->far_plane();
    const float tan_half = std::tan(math::to_radians(camera_->fov()) * 0.5f);
    const float aspect = camera_->aspect();
    const math::Vector3f pos = camera_->position();
    const math::Vector3f fwd = camera_->forward();
    const math::Vector3f rgt = camera_->right();
    const math::Vector3f upv = camera_->up();

    // 视锥角点（世界空间）
    auto corner = [&](float dist, float sx, float sy) {
        const float hh = tan_half * dist;
        const float hw = hh * aspect;
        return pos + fwd * dist + rgt * (hw * sx) + upv * (hh * sy);
    };

    // 级联分割：practical split scheme（线性与对数分布按 lambda 插值）。
    // split[i] 是第 i 级联的远平面（相机空间距离），split[0]=near。
    const int n = cascade_count_;
    cascade_split_distances_[0] = cam_near;
    for (int i = 1; i <= n; ++i) {
        const float t_log = cam_near * std::pow(cam_far / cam_near,
                                                static_cast<float>(i) / static_cast<float>(n));
        const float t_lin = cam_near + (cam_far - cam_near) *
                                           (static_cast<float>(i) / static_cast<float>(n));
        cascade_split_distances_[i] = math::lerp(t_lin, t_log, cascade_split_lambda_);
    }

    // 固定 light view：以整视锥中心为 look_at 目标，所有级联共用同一朝向，
    // 保证级联之间无缝衔接。
    math::Vector3f full_center = math::Vector3f::zero();
    for (int k = 0; k < 8; ++k) {
        full_center = full_center + corner(cam_far,
                                           (k & 1) ? 1.0f : -1.0f,
                                           (k & 2) ? 1.0f : -1.0f);
    }
    full_center = full_center / 8.0f;
    float frustum_radius = 0.0f;
    for (int k = 0; k < 8; ++k) {
        math::Vector3f c = corner(cam_far, (k & 1) ? 1.0f : -1.0f, (k & 2) ? 1.0f : -1.0f);
        frustum_radius = std::max(frustum_radius, (c - full_center).length());
    }
    const float eye_dist = frustum_radius + 60.0f;
    const math::Vector3f eye = full_center - light_dir * eye_dist;
    const math::Matrix4f light_view = math::Matrix4f::look_at(eye, full_center, up);

    for (int c = 0; c < n; ++c) {
        const float near_d = cascade_split_distances_[c];
        const float far_d = cascade_split_distances_[c + 1];

        // 该级联的视锥切片 8 角点
        math::Vector3f slice[8] = {
            corner(near_d, -1.0f, -1.0f), corner(near_d, 1.0f, -1.0f),
            corner(near_d, 1.0f, 1.0f),   corner(near_d, -1.0f, 1.0f),
            corner(far_d, -1.0f, -1.0f),  corner(far_d, 1.0f, -1.0f),
            corner(far_d, 1.0f, 1.0f),    corner(far_d, -1.0f, 1.0f),
        };

        math::Vector3f lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
        for (const auto& p : slice) {
            math::Vector3f lp = light_view.transform_point(p);
            lo.x = std::min(lo.x, lp.x); lo.y = std::min(lo.y, lp.y); lo.z = std::min(lo.z, lp.z);
            hi.x = std::max(hi.x, lp.x); hi.y = std::max(hi.y, lp.y); hi.z = std::max(hi.z, lp.z);
        }

        // 轻微外扩，避免视锥边缘恰好贴盒边采样出问题
        const float margin_x = (hi.x - lo.x) * 0.02f + 0.5f;
        const float margin_y = (hi.y - lo.y) * 0.02f + 0.5f;
        lo.x -= margin_x; hi.x += margin_x;
        lo.y -= margin_y; hi.y += margin_y;

        // Texel Snapping：盒尺寸保持不变（稳定），起点对齐 texel 网格（消除移动抖动）
        const float texel_x = (hi.x - lo.x) / static_cast<float>(cascade_sizes_[c]);
        const float texel_y = (hi.y - lo.y) / static_cast<float>(cascade_sizes_[c]);
        if (texel_x > 1e-6f) {
            const float snapped = std::floor(lo.x / texel_x) * texel_x;
            hi.x = snapped + (hi.x - lo.x);
            lo.x = snapped;
        }
        if (texel_y > 1e-6f) {
            const float snapped = std::floor(lo.y / texel_y) * texel_y;
            hi.y = snapped + (hi.y - lo.y);
            lo.y = snapped;
        }

        // 深度范围：near 朝光源方向延伸，把视锥外（画面上方/侧面）的投影物纳入，
        // far 留一点余量。
        const float near_plane = std::max(0.1f, -hi.z - 50.0f);
        const float far_plane = -lo.z + 10.0f;

        math::Matrix4f light_proj = math::Matrix4f::ortho(lo.x, hi.x, lo.y, hi.y, near_plane, far_plane);
        cascade_light_space_matrices_[c] = light_proj * light_view;
        cascade_texel_sizes_[c] = std::max((hi.x - lo.x), (hi.y - lo.y)) /
                                  static_cast<float>(cascade_sizes_[c]);
    }

    // 级联 0 同时作为兼容入口（旧 SPIR-V / 单级联路径使用）
    light_space_matrix_ = cascade_light_space_matrices_[0];
}

// ---------------------------------------------------------------------------
// Frustum culling
// ---------------------------------------------------------------------------

void RenderPipeline::begin_shadow_pass(RenderContext& ctx, int cascade) {
    const int size = cascade_sizes_[cascade];
    ctx.set_shader(shadow_shader_);
    ctx.set_framebuffer(shadow_fbos_[cascade]);
    ctx.set_viewport(0, 0, size, size);
    // VulkanBackend::set_viewport 会同步设置 scissor，无需额外调用。
    ctx.clear_depth();
    ctx.set_depth_test(true);
    ctx.set_depth_write(true);
    ctx.set_cull_face(cull_disabled_ ? CullMode::None : CullMode::Back);
    // Normal Offset Shadow Mapping：沿法线把几何推向光源（每级按自己的 texel 尺寸）
    ctx.set_uniform_float(shadow_shader_, "uNormalOffset",
                          cascade_texel_sizes_[cascade] * normal_offset_scale_);
}


void RenderPipeline::end_shadow_pass(RenderContext& ctx) {
    ctx.set_framebuffer(RHIFramebufferHandle{});
}


} // namespace gryce_engine::render
