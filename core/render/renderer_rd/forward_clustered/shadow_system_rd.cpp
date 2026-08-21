#include "render/renderer_rd/forward_clustered/shadow_system_rd.h"
#include "render/render_context.h"
#include "render/texture.h"
#include "render/framebuffer.h"
#include "render/shader.h"
#include "render/mesh.h"
#include "utils/glog/glog_lib.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace gryce_engine::render {

ShadowSystemRD::ShadowSystemRD() = default;

ShadowSystemRD::~ShadowSystemRD() {
    shutdown();
}

bool ShadowSystemRD::init(RenderContext* ctx) {
    if (initialized_) return true;
    ctx_ = ctx;

    // 创建级联阴影贴图
    for (int i = 0; i < k_max_cascades; ++i) {
        const int size = cascade_sizes_[i];

        cascade_shadow_tex_[i] = ctx->create_texture();
        ITexture* tex = ctx->texture(cascade_shadow_tex_[i]);
        if (!cascade_shadow_tex_[i].is_valid() || !tex ||
            !tex->create_depth(size, size)) {
            GLOG_ERROR("ShadowSystem: failed to create cascade {} shadow tex ({}x{})", i, size, size);
            return false;
        }
        tex->set_filter(TextureFilter::Linear, TextureFilter::Linear);
        tex->set_wrap(TextureWrap::ClampToBorder, TextureWrap::ClampToBorder);

        cascade_shadow_fbo_[i] = ctx->create_framebuffer();
        IFramebuffer* fbo = ctx->framebuffer(cascade_shadow_fbo_[i]);
        if (!cascade_shadow_fbo_[i].is_valid() || !fbo || !fbo->create(size, size)) {
            return false;
        }
        fbo->attach_depth_texture(tex);
        if (!fbo->is_complete()) {
            GLOG_ERROR("ShadowSystem: cascade {} FBO incomplete", i);
            return false;
        }
    }

    initialized_ = true;
    GLOG_INFO("ShadowSystemRD initialized ({} cascades, max {}x{})",
              k_max_cascades, cascade_sizes_[0], cascade_sizes_[0]);
    return true;
}

void ShadowSystemRD::shutdown() {
    if (!initialized_) return;

    for (int i = 0; i < k_max_cascades; ++i) {
        if (cascade_shadow_fbo_[i].is_valid()) {
            ctx_->destroy_framebuffer(cascade_shadow_fbo_[i]);
            cascade_shadow_fbo_[i] = {};
        }
        if (cascade_shadow_tex_[i].is_valid()) {
            ctx_->destroy_texture(cascade_shadow_tex_[i]);
            cascade_shadow_tex_[i] = {};
        }
    }

    for (auto& fbo : spot_shadow_fbo_) {
        if (fbo.is_valid()) ctx_->destroy_framebuffer(fbo);
    }
    for (auto& tex : spot_shadow_tex_) {
        if (tex.is_valid()) ctx_->destroy_texture(tex);
    }
    spot_shadow_fbo_.clear();
    spot_shadow_tex_.clear();
    spot_light_matrices_.clear();

    // 清理点光源阴影
    for (auto& fbo_pair : point_shadow_fbo_) {
        for (auto& fbo : fbo_pair) {
            if (fbo.is_valid()) ctx_->destroy_framebuffer(fbo);
        }
    }
    for (auto& tex : point_shadow_tex_) {
        if (tex.is_valid()) ctx_->destroy_texture(tex);
    }
    point_shadow_fbo_.clear();
    point_shadow_tex_.clear();
    point_light_view_matrices_.clear();
    point_light_positions_.clear();

    current_shadow_lights_.clear();
    initialized_ = false;
}

// ---------------------------------------------------------------------------
// 计算级联分割距离：使用 Practical Split Scheme (PSSM)
// lambda 控制对数/均匀混合比例
// ---------------------------------------------------------------------------
void ShadowSystemRD::_compute_cascade_splits(const math::Camera& camera) {
    const float near_plane = camera.near_plane();
    const float far_plane = camera.far_plane();
    const float lambda = cascade_split_lambda_;

    cascade_splits_[0] = near_plane;
    for (int i = 1; i <= cascade_count_; ++i) {
        float fraction = static_cast<float>(i) / cascade_count_;
        // 对数分割：更多级联分配给近处
        float log_split = near_plane * std::pow(far_plane / near_plane, fraction);
        // 均匀分割
        float uniform_split = near_plane + (far_plane - near_plane) * fraction;
        // 混合（Practical Split Scheme）
        cascade_splits_[i] = log_split * lambda + uniform_split * (1.0f - lambda);
    }
}

// ---------------------------------------------------------------------------
// 计算单个级联的光照矩阵（稳定 CSM 算法）
// 使用稳定拟合：光照矩阵在级联之间平滑变化
// ---------------------------------------------------------------------------
math::Matrix4f ShadowSystemRD::_compute_cascade_light_matrix(
    const math::Camera& camera, int cascade,
    const math::Vector3f& light_dir)
{
    // 获取级联的观察空间分割距离
    const float split_near = cascade_splits_[cascade];
    const float split_far = cascade_splits_[cascade + 1];

    // 计算视锥体在观察空间中的 8 个角点
    const math::Matrix4f inv_view_proj = (camera.get_projection_matrix() * camera.get_view_matrix()).inverse();

    const float tan_half_fov = std::tan(math::to_radians(camera.fov()) * 0.5f);
    const float aspect = camera.aspect();

    // 计算近/远平面处的视锥体半宽高
    const float near_half_h = tan_half_fov * split_near;
    const float near_half_w = near_half_h * aspect;
    const float far_half_h = tan_half_fov * split_far;
    const float far_half_w = far_half_h * aspect;

    // 视锥体 8 个角点（观察空间）
    math::Vector3f frustum_corners[8];
    // 近平面
    frustum_corners[0] = math::Vector3f(-near_half_w, -near_half_h, -split_near);
    frustum_corners[1] = math::Vector3f( near_half_w, -near_half_h, -split_near);
    frustum_corners[2] = math::Vector3f(-near_half_w,  near_half_h, -split_near);
    frustum_corners[3] = math::Vector3f( near_half_w,  near_half_h, -split_near);
    // 远平面
    frustum_corners[4] = math::Vector3f(-far_half_w, -far_half_h, -split_far);
    frustum_corners[5] = math::Vector3f( far_half_w, -far_half_h, -split_far);
    frustum_corners[6] = math::Vector3f(-far_half_w,  far_half_h, -split_far);
    frustum_corners[7] = math::Vector3f( far_half_w,  far_half_h, -split_far);

    // 变换到世界空间
    const math::Matrix4f view_inv = camera.get_view_matrix().inverse();
    for (int i = 0; i < 8; ++i) {
        math::Vector4f world_pt = view_inv * math::Vector4f(frustum_corners[i].x, frustum_corners[i].y, frustum_corners[i].z, 1.0f);
        frustum_corners[i] = math::Vector3f(world_pt.x / world_pt.w, world_pt.y / world_pt.w, world_pt.z / world_pt.w);
    }

    // 计算视锥体包围盒中心
    math::Vector3f center(0.0f, 0.0f, 0.0f);
    for (int i = 0; i < 8; ++i) {
        center = center + frustum_corners[i];
    }
    center = center * (1.0f / 8.0f);

    // 构建光照视图矩阵（看向场景中心）
    math::Vector3f up = math::Vector3f(0.0f, 1.0f, 0.0f);
    // 如果光照方向与 up 平行，使用 forward 作为 up
    if (std::abs(light_dir.dot(up)) > 0.99f) {
        up = math::Vector3f(0.0f, 0.0f, 1.0f);
    }
    math::Matrix4f light_view = math::Matrix4f::look_at(center + light_dir * 100.0f, center, up);

    // 将视锥体角点变换到光照空间，计算包围盒
    math::Vector3f light_space_min(FLT_MAX, FLT_MAX, FLT_MAX);
    math::Vector3f light_space_max(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    for (int i = 0; i < 8; ++i) {
        math::Vector4f ls_pt = light_view * math::Vector4f(frustum_corners[i].x, frustum_corners[i].y, frustum_corners[i].z, 1.0f);
        light_space_min = light_space_min.min(math::Vector3f(ls_pt.x, ls_pt.y, ls_pt.z));
        light_space_max = light_space_max.max(math::Vector3f(ls_pt.x, ls_pt.y, ls_pt.z));
    }

    // 稳定 CSM：对齐到 texel 边界，避免级联边缘闪烁
    const float texel_size_x = (light_space_max.x - light_space_min.x) / cascade_sizes_[cascade];
    const float texel_size_y = (light_space_max.y - light_space_min.y) / cascade_sizes_[cascade];
    const float texel_size_z = (light_space_max.z - light_space_min.z) / cascade_sizes_[cascade];

    // 将包围盒对齐到 texel 网格
    auto snap_to_texel = [](float val, float texel_size) -> float {
        return std::floor(val / texel_size) * texel_size;
    };

    light_space_min.x = snap_to_texel(light_space_min.x, texel_size_x);
    light_space_min.y = snap_to_texel(light_space_min.y, texel_size_y);
    light_space_min.z = snap_to_texel(light_space_min.z, texel_size_z);
    light_space_max.x = light_space_min.x + (light_space_max.x - light_space_min.x);
    light_space_max.y = light_space_min.y + (light_space_max.y - light_space_min.y);
    light_space_max.z = light_space_min.z + (light_space_max.z - light_space_min.z);

    // 构建正交投影矩阵
    math::Matrix4f light_proj = math::Matrix4f::ortho(
        light_space_min.x, light_space_max.x,
        light_space_min.y, light_space_max.y,
        light_space_min.z, light_space_max.z);

    return light_proj * light_view;
}

// ---------------------------------------------------------------------------
// 渲染单个级联阴影
// ---------------------------------------------------------------------------
void ShadowSystemRD::_render_cascade(RenderContext* ctx, int cascade,
                                      const math::Vector3f& light_dir)
{
    (void)light_dir;
    if (!cascade_shadow_fbo_[cascade].is_valid()) return;

    const int size = cascade_sizes_[cascade];

    ctx->set_framebuffer(cascade_shadow_fbo_[cascade]);
    ctx->set_viewport(0, 0, size, size);
    ctx->clear_depth();
    ctx->set_depth_test(true);
    ctx->set_depth_write(true);
    ctx->set_cull_face(CullMode::Front);

    // 设置光照矩阵
    const math::Matrix4f light_mvp = cascade_light_matrices_[cascade];

    // 渲染场景中的物体到阴影贴图
    // 注意：实际渲染由外部调用方提供物体列表
    // 这里只做 FBO 设置和清理
    // 实际的物体渲染在 RenderForwardClustered::_render_shadows 中完成

    ctx->set_cull_face(CullMode::None);
}

// ---------------------------------------------------------------------------
// 渲染单个聚光灯阴影
// ---------------------------------------------------------------------------
void ShadowSystemRD::_render_spot_shadow(RenderContext* ctx, int index,
                                          const ShadowLight& light)
{
    if (index >= static_cast<int>(spot_shadow_fbo_.size())) return;
    if (!spot_shadow_fbo_[index].is_valid()) return;

    const int size = spot_shadow_size_;

    ctx->set_framebuffer(spot_shadow_fbo_[index]);
    ctx->set_viewport(0, 0, size, size);
    ctx->clear_depth();
    ctx->set_depth_test(true);
    ctx->set_depth_write(true);
    ctx->set_cull_face(CullMode::Front);

    // 计算聚光灯的光照矩阵（透视投影）
    const float half_angle = light.spot_angle * 0.5f;
    const float aspect = 1.0f;  // 正方形阴影贴图
    math::Matrix4f spot_proj = math::Matrix4f::perspective(half_angle * 2.0f, aspect, 0.1f, light.range);

    math::Vector3f up = math::Vector3f(0.0f, 1.0f, 0.0f);
    if (std::abs(light.direction.dot(up)) > 0.99f) {
        up = math::Vector3f(0.0f, 0.0f, 1.0f);
    }
    math::Matrix4f spot_view = math::Matrix4f::look_at(light.position, light.position + light.direction, up);
    spot_light_matrices_[index] = spot_proj * spot_view;

    ctx->set_cull_face(CullMode::None);
}

// ---------------------------------------------------------------------------
// 每帧更新：计算级联分割、光照矩阵，创建/更新阴影贴图
// ---------------------------------------------------------------------------
void ShadowSystemRD::update(const math::Camera& camera,
                             const std::vector<ShadowLight>& shadow_lights,
                             int viewport_width, int viewport_height)
{
    (void)viewport_width;
    (void)viewport_height;
    current_shadow_lights_ = shadow_lights;
    light_matrices_.clear();

    // 找到第一个方向光（用于 CSM）
    has_directional_shadow_ = false;
    for (const auto& sl : shadow_lights) {
        if (sl.type == LightType::Directional && sl.shadow_enabled) {
            current_light_dir_ = sl.direction;
            has_directional_shadow_ = true;
            break;
        }
    }

    if (has_directional_shadow_) {
        // 计算级联分割
        _compute_cascade_splits(camera);

        // 计算每级级联的光照矩阵
        for (int i = 0; i < cascade_count_; ++i) {
            cascade_light_matrices_[i] = _compute_cascade_light_matrix(camera, i, current_light_dir_);
            light_matrices_.push_back(cascade_light_matrices_[i]);
        }
    }

    // 更新聚光灯阴影贴图
    // 先清空旧资源
    for (auto& fbo : spot_shadow_fbo_) {
        if (fbo.is_valid()) ctx_->destroy_framebuffer(fbo);
    }
    for (auto& tex : spot_shadow_tex_) {
        if (tex.is_valid()) ctx_->destroy_texture(tex);
    }
    spot_shadow_fbo_.clear();
    spot_shadow_tex_.clear();
    spot_light_matrices_.clear();

    // 为每个聚光灯创建阴影贴图
    for (const auto& sl : shadow_lights) {
        if (sl.type != LightType::Spot || !sl.shadow_enabled) continue;

        RHITextureHandle tex = ctx_->create_texture();
        ITexture* tex_ptr = ctx_->texture(tex);
        if (!tex.is_valid() || !tex_ptr ||
            !tex_ptr->create_depth(spot_shadow_size_, spot_shadow_size_)) {
            GLOG_ERROR("ShadowSystem: failed to create spot shadow tex");
            continue;
        }
        tex_ptr->set_filter(TextureFilter::Linear, TextureFilter::Linear);
        tex_ptr->set_wrap(TextureWrap::ClampToBorder, TextureWrap::ClampToBorder);

        RHIFramebufferHandle fbo = ctx_->create_framebuffer();
        IFramebuffer* fbo_ptr = ctx_->framebuffer(fbo);
        if (!fbo.is_valid() || !fbo_ptr || !fbo_ptr->create(spot_shadow_size_, spot_shadow_size_)) {
            ctx_->destroy_texture(tex);
            continue;
        }
        fbo_ptr->attach_depth_texture(tex_ptr);
        if (!fbo_ptr->is_complete()) {
            ctx_->destroy_texture(tex);
            ctx_->destroy_framebuffer(fbo);
            continue;
        }

        spot_shadow_tex_.push_back(tex);
        spot_shadow_fbo_.push_back(fbo);
        spot_light_matrices_.emplace_back();
    }

    // 更新点光源阴影贴图（双抛物面映射）
    // 先清空旧资源
    for (auto& fbo_pair : point_shadow_fbo_) {
        for (auto& fbo : fbo_pair) {
            if (fbo.is_valid()) ctx_->destroy_framebuffer(fbo);
        }
    }
    for (auto& tex : point_shadow_tex_) {
        if (tex.is_valid()) ctx_->destroy_texture(tex);
    }
    point_shadow_fbo_.clear();
    point_shadow_tex_.clear();
    point_light_view_matrices_.clear();
    point_light_positions_.clear();

    // 为每个点光源创建阴影贴图（2个face共用1张纹理，各1个FBO）
    for (const auto& sl : shadow_lights) {
        if (sl.type != LightType::Point || !sl.shadow_enabled) continue;

        RHITextureHandle tex = ctx_->create_texture();
        ITexture* tex_ptr = ctx_->texture(tex);
        if (!tex.is_valid() || !tex_ptr ||
            !tex_ptr->create_depth(point_shadow_size_, point_shadow_size_)) {
            GLOG_ERROR("ShadowSystem: failed to create point shadow tex");
            continue;
        }
        tex_ptr->set_filter(TextureFilter::Linear, TextureFilter::Linear);
        tex_ptr->set_wrap(TextureWrap::ClampToBorder, TextureWrap::ClampToBorder);

        // 创建2个face的FBO
        std::array<RHIFramebufferHandle, 2> fbo_pair = {};
        for (int f = 0; f < 2; ++f) {
            RHIFramebufferHandle fbo = ctx_->create_framebuffer();
            IFramebuffer* fbo_ptr = ctx_->framebuffer(fbo);
            if (!fbo.is_valid() || !fbo_ptr || !fbo_ptr->create(point_shadow_size_, point_shadow_size_)) {
                if (fbo.is_valid()) ctx_->destroy_framebuffer(fbo);
                continue;
            }
            fbo_ptr->attach_depth_texture(tex_ptr);
            if (!fbo_ptr->is_complete()) {
                ctx_->destroy_framebuffer(fbo);
                continue;
            }
            fbo_pair[f] = fbo;
        }
        if (!fbo_pair[0].is_valid() && !fbo_pair[1].is_valid()) {
            ctx_->destroy_texture(tex);
            continue;
        }

        point_shadow_tex_.push_back(tex);
        point_shadow_fbo_.push_back(fbo_pair);
        point_light_view_matrices_.emplace_back();
        point_light_positions_.push_back(sl.position);
    }
}

// ---------------------------------------------------------------------------
// 渲染所有阴影贴图
// ---------------------------------------------------------------------------
void ShadowSystemRD::render_shadows(RenderContext* ctx) {
    if (!has_directional_shadow_) return;

    // 渲染 CSM 级联阴影
    for (int i = 0; i < cascade_count_; ++i) {
        _render_cascade(ctx, i, current_light_dir_);
    }

    // 渲染聚光灯阴影
    for (size_t i = 0; i < spot_shadow_fbo_.size(); ++i) {
        _render_spot_shadow(ctx, static_cast<int>(i), current_shadow_lights_[i]);
    }

    // 渲染点光源阴影（双抛物面映射）
    for (size_t i = 0; i < point_shadow_fbo_.size(); ++i) {
        for (int f = 0; f < 2; ++f) {
            if (point_shadow_fbo_[i][f].is_valid()) {
                _render_point_shadow_face(ctx, static_cast<int>(i), f, current_shadow_lights_[i]);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// 渲染点光源阴影 face（双抛物面映射）
// face=0: +Z 方向（前半球），face=1: -Z 方向（后半球）
// 使用双抛物面映射只需一张 depth texture，但需要 2 个不同的 view 矩阵。
// ---------------------------------------------------------------------------
void ShadowSystemRD::_render_point_shadow_face(RenderContext* ctx, int index,
                                                int face, const ShadowLight& light)
{
    if (index >= static_cast<int>(point_shadow_fbo_.size())) return;
    if (!point_shadow_fbo_[index][face].is_valid()) return;

    const int size = point_shadow_size_;

    ctx->set_framebuffer(point_shadow_fbo_[index][face]);
    ctx->set_viewport(0, 0, size, size);
    ctx->clear_depth();
    ctx->set_depth_test(true);
    ctx->set_depth_write(true);
    ctx->set_cull_face(CullMode::Front);

    // 计算双抛物面映射的 view 矩阵
    // face=0: 看向 +Z，face=1: 看向 -Z
    math::Vector3f dir = (face == 0) ? math::Vector3f(0.0f, 0.0f, 1.0f) : math::Vector3f(0.0f, 0.0f, -1.0f);
    math::Vector3f up = math::Vector3f(0.0f, 1.0f, 0.0f);
    if (std::abs(dir.dot(up)) > 0.99f) {
        up = math::Vector3f(0.0f, 0.0f, 1.0f);
    }
    math::Matrix4f view = math::Matrix4f::look_at(light.position, light.position + dir, up);
    point_light_view_matrices_[index][face] = view;

    // 使用 90 度 FOV 的正交投影（双抛物面映射使用 90 度视角）
    float half_w = light.range * 2.0f;
    math::Matrix4f proj = math::Matrix4f::ortho(-half_w, half_w, -half_w, half_w, 0.01f, light.range * 2.0f);
    math::Matrix4f light_mvp = proj * view;

    // 将光照矩阵（用于 shader 中重建世界空间位置）拼接到 light_matrices_ 中
    // 注：实际渲染由外部调用方提供物体列表
    // 这里只做 FBO 设置和清理，物体渲染在 RenderForwardClustered::_render_shadows 中完成

    ctx->set_cull_face(CullMode::None);
}

} // namespace gryce_engine::render