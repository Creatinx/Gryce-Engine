#include "render/renderer_rd/forward_clustered/render_forward_clustered.h"
#include "render/render_context.h"
#include "render/render.h"
#include "render/texture.h"
#include "render/framebuffer.h"
#include "render/mesh.h"
#include "render/material.h"
#include "render/buffer.h"
#include "render/storage_rd/light_storage.h"
#include "scene/scene.h"
#include "scene/entity.h"
#include "components/transform.h"
#include "components/light.h"
#include "components/mesh_renderer.h"
#include "components/skinned_mesh_renderer.h"
#include "components/decal.h"
#include "scene/query.h"
#include "math/camera.h"
#include "utils/glog/glog_lib.h"

#include <algorithm>
#include <cmath>

namespace gryce_engine::render {

// ---------------------------------------------------------------------------
// RenderForwardClustered — 实现
// ---------------------------------------------------------------------------

RenderForwardClustered::RenderForwardClustered()
    : cluster_builder_(std::make_unique<ClusterBuilderRD>())
    , shader_system_(std::make_unique<SceneShaderForwardClustered>())
    , shadow_system_(std::make_unique<ShadowSystemRD>()) {
}

RenderForwardClustered::~RenderForwardClustered() {
    shutdown();
}

bool RenderForwardClustered::init(RenderContext* ctx, const std::string& shader_dir) {
    if (initialized_) return true;
    ctx_ = ctx;

    // 初始化子组件
    if (!cluster_builder_->init(ctx)) {
        GLOG_ERROR("RenderForwardClustered: failed to init ClusterBuilder");
        return false;
    }

    if (!shader_system_->init(ctx, shader_dir)) {
        GLOG_ERROR("RenderForwardClustered: failed to init SceneShader");
        return false;
    }

    // 初始化阴影系统
    if (!shadow_system_->init(ctx)) {
        GLOG_ERROR("RenderForwardClustered: failed to init ShadowSystem");
        return false;
    }

    // 初始化后处理效果
    ssr_ = std::make_unique<SSR_RD>();
    if (!ssr_->init(ctx)) {
        GLOG_ERROR("RenderForwardClustered: failed to init SSR");
        return false;
    }

    ssil_ = std::make_unique<SSIL_RD>();
    if (!ssil_->init(ctx)) {
        GLOG_ERROR("RenderForwardClustered: failed to init SSIL");
        return false;
    }

    bokeh_dof_ = std::make_unique<BokehDOF_RD>();
    if (!bokeh_dof_->init(ctx)) {
        GLOG_ERROR("RenderForwardClustered: failed to init BokehDOF");
        return false;
    }

    fsr2_ = std::make_unique<FSR2_RD>();
    if (!fsr2_->init(ctx)) {
        GLOG_ERROR("RenderForwardClustered: failed to init FSR2");
        return false;
    }

    // 初始化 SSS
    subsurface_scattering_ = std::make_unique<SubsurfaceScattering_RD>();
    if (!subsurface_scattering_->init(ctx, shader_dir)) {
        GLOG_ERROR("RenderForwardClustered: failed to init SubsurfaceScattering");
        return false;
    }

    // 初始化体积雾
    fog_ = std::make_unique<VolumetricFog_RD>();
    if (!fog_->init(ctx, shader_dir)) {
        GLOG_ERROR("RenderForwardClustered: failed to init VolumetricFog");
        return false;
    }

    // 初始化 SDFGI
    sdfgi_ = std::make_unique<SDFGI_RD>();
    if (!sdfgi_->init(ctx)) {
        GLOG_ERROR("RenderForwardClustered: failed to init SDFGI");
        return false;
    }
    sdfgi_->create_cascades(math::Vector3f::zero(), 64.0f);

    // 初始化反射探针
    reflection_probes_ = std::make_unique<ReflectionProbeRD>();
    if (!reflection_probes_->init(ctx)) {
        GLOG_ERROR("RenderForwardClustered: failed to init ReflectionProbes");
        return false;
    }

    // 初始化 shader groups
    shader_system_->enable_group(SceneShaderForwardClustered::SHADER_GROUP_BASE);
    shader_system_->enable_group(SceneShaderForwardClustered::SHADER_GROUP_DEPTH_PREPASS);
    shader_system_->enable_group(SceneShaderForwardClustered::SHADER_GROUP_MOTION_VECTORS);

    // 创建默认大小的 framebuffer
    if (!_create_internal_framebuffers(1280, 720)) {
        return false;
    }

    // 初始化天空盒 shader
    std::string sky_vs = shader_dir + "/skybox.vert";
    std::string sky_fs = shader_dir + "/skybox.frag";
    skybox_shader_ = ctx->create_shader();
    if (skybox_shader_.is_valid()) {
        IShader* sky_shader = ctx->shader(skybox_shader_);
        if (sky_shader) {
            skybox_initialized_ = sky_shader->load_program(sky_vs.c_str(), sky_fs.c_str());
            if (!skybox_initialized_) {
                GLOG_WARN("RenderForwardClustered: failed to load skybox shader");
            }
        }
    }

    // 初始化贴花 shader 和存储
    decal_storage_ = std::make_unique<DecalStorage>();
    decal_storage_->init(ctx);
    std::string decal_vs = shader_dir + "/decal.vert";
    std::string decal_fs = shader_dir + "/decal.frag";
    decal_shader_ = ctx->create_shader();
    if (decal_shader_.is_valid()) {
        IShader* dshader = ctx->shader(decal_shader_);
        if (dshader) {
            decal_initialized_ = dshader->load_program(decal_vs.c_str(), decal_fs.c_str());
            if (!decal_initialized_) {
                GLOG_WARN("RenderForwardClustered: failed to load decal shader");
            }
        }
    }
    // 创建贴花盒体 mesh（单位立方体）
    decal_box_mesh_ = ctx->create_mesh();
    if (decal_box_mesh_.is_valid()) {
        IMesh* mesh = ctx->mesh(decal_box_mesh_);
        if (mesh) {
            float box_verts[] = {
                -0.5f, -0.5f, -0.5f,  0.5f, -0.5f, -0.5f,  0.5f,  0.5f, -0.5f, -0.5f,  0.5f, -0.5f,
                -0.5f, -0.5f,  0.5f,  0.5f, -0.5f,  0.5f,  0.5f,  0.5f,  0.5f, -0.5f,  0.5f,  0.5f,
            };
            uint32_t box_indices[] = {
                0,1,2, 0,2,3, 1,5,6, 1,6,2, 5,4,7, 5,7,6, 4,0,3, 4,3,7, 3,2,6, 3,6,7, 4,5,1, 4,1,0
            };
            VertexLayout layout;
            layout.stride = 3 * sizeof(float);
            layout.attributes = { {0, VertexType::Float3, false, 0} };
            mesh->set_layout(layout);
            mesh->upload_vertices(box_verts, 3 * sizeof(float), 8);
            mesh->upload_indices(box_indices, sizeof(uint32_t), 36);
        }
    }

    initialized_ = true;
    GLOG_INFO("RenderForwardClustered initialized");
    return true;
}

void RenderForwardClustered::shutdown() {
    if (!initialized_) return;

    ssr_.reset();
    ssil_.reset();
    bokeh_dof_.reset();
    fsr2_.reset();
    subsurface_scattering_.reset();
    fog_.reset();
    sdfgi_.reset();
    reflection_probes_.reset();

    _destroy_bloom_targets();
    _destroy_ssao_targets();
    _destroy_taa_targets();
    _destroy_auto_exposure_targets();

    // 销毁内部 framebuffer
    if (fb_.color_fbo.is_valid()) { ctx_->destroy_framebuffer(fb_.color_fbo); fb_.color_fbo = {}; }
    if (fb_.depth_fbo.is_valid()) { ctx_->destroy_framebuffer(fb_.depth_fbo); fb_.depth_fbo = {}; }
    if (fb_.depth_normal_fbo.is_valid()) { ctx_->destroy_framebuffer(fb_.depth_normal_fbo); fb_.depth_normal_fbo = {}; }
    if (fb_.motion_vectors_fbo.is_valid()) { ctx_->destroy_framebuffer(fb_.motion_vectors_fbo); fb_.motion_vectors_fbo = {}; }
    if (fb_.viewport_fbo.is_valid()) { ctx_->destroy_framebuffer(fb_.viewport_fbo); fb_.viewport_fbo = {}; }

    if (fb_.color_tex.is_valid()) { ctx_->destroy_texture(fb_.color_tex); fb_.color_tex = {}; }
    if (fb_.depth_tex.is_valid()) { ctx_->destroy_texture(fb_.depth_tex); fb_.depth_tex = {}; }
    if (fb_.depth_normal_tex.is_valid()) { ctx_->destroy_texture(fb_.depth_normal_tex); fb_.depth_normal_tex = {}; }
    if (fb_.motion_vectors_tex.is_valid()) { ctx_->destroy_texture(fb_.motion_vectors_tex); fb_.motion_vectors_tex = {}; }
    if (fb_.viewport_tex.is_valid()) { ctx_->destroy_texture(fb_.viewport_tex); fb_.viewport_tex = {}; }

    if (fullscreen_mesh_.is_valid()) { ctx_->destroy_mesh(fullscreen_mesh_); fullscreen_mesh_ = {}; }

    // 销毁天空盒资源
    if (skybox_shader_.is_valid()) { ctx_->destroy_shader(skybox_shader_); skybox_shader_ = {}; }
    if (skybox_tex_.is_valid()) { ctx_->destroy_texture(skybox_tex_); skybox_tex_ = {}; }
    skybox_initialized_ = false;

    // 销毁贴花资源
    if (decal_shader_.is_valid()) { ctx_->destroy_shader(decal_shader_); decal_shader_ = {}; }
    if (decal_box_mesh_.is_valid()) { ctx_->destroy_mesh(decal_box_mesh_); decal_box_mesh_ = {}; }
    if (decal_storage_) { decal_storage_->destroy(); decal_storage_.reset(); }
    decal_initialized_ = false;

    cluster_builder_->shutdown();
    shader_system_->shutdown();
    shadow_system_->shutdown();
    initialized_ = false;
}

// ---------------------------------------------------------------------------
// 渲染主入口
// ---------------------------------------------------------------------------
void RenderForwardClustered::render_scene(RenderData& data) {
    if (!initialized_ || !data.scene || !data.camera) return;

    // 保存渲染数据快照
    current_camera_ = *data.camera;
    current_scene_ = data.scene;
    viewport_width_ = data.viewport_width;
    viewport_height_ = data.viewport_height;
    delta_time_ = data.delta_time;
    frame_index_ = data.frame_index;
    ambient_light_ = data.ambient_light;

    // 确保 framebuffer 大小匹配
    if (fb_.width != viewport_width_ || fb_.height != viewport_height_) {
        _create_internal_framebuffers(viewport_width_, viewport_height_);
    }

    // 1. 构建集群
    _setup_cluster(data);

    // 2. 填充渲染列表（必须在阴影渲染之前，因为阴影需要几何体列表）
    _populate_render_lists(data);

    // 3. 更新阴影系统（计算级联分割、光照矩阵）
    {
        // 收集阴影光源数据
        std::vector<ShadowSystemRD::ShadowLight> shadow_lights;
        for (const auto& ld : scene_lights_) {
            if (!ld.shadow_enabled) continue;
            ShadowSystemRD::ShadowLight sl;
            sl.type = ld.type;
            sl.position = ld.position;
            sl.direction = ld.direction;
            sl.range = ld.range;
            sl.spot_angle = ld.spot_angle;
            sl.spot_softness = ld.spot_softness;
            sl.bias = 0.001f;
            sl.normal_bias = 0.001f;
            sl.shadow_enabled = true;
            shadow_lights.push_back(sl);
        }
        shadow_system_->update(current_camera_, shadow_lights,
                               viewport_width_, viewport_height_);
    }

    // 4. 渲染阴影贴图
    _render_shadows(ctx_);

    // 5. Depth Prepass
    _render_depth_prepass(ctx_);

    // 6. 运动向量 pass
    if (frame_index_ > 0) {
        _render_motion_vectors(ctx_);
    }

    // 7. 不透明 pass
    ctx_->set_framebuffer(fb_.color_fbo);
    ctx_->clear(0.0f, 0.0f, 0.0f, 1.0f);
    ctx_->clear_depth();
    _render_opaque_pass(ctx_);

    // 8. 贴花（投影到不透明表面，使用深度缓冲重建世界位置）
    _render_decals(ctx_);

    // 9. 天空盒
    _render_sky(ctx_);

    // 10. 透明 pass
    sort_render_list(RENDER_LIST_ALPHA);
    _render_alpha_pass(ctx_);

    // 11. 体积雾（渲染 fog 体积并合成到场景颜色）
    if (fog_ && fog_->valid()) {
        math::Matrix4f view = current_camera_.get_view_matrix();
        math::Matrix4f proj = current_camera_.get_projection_matrix();
        math::Matrix4f inv_vp = (proj * view).inverse();

        // 先渲染 fog 体积到内部纹理
        fog_->render(ctx_, fb_.depth_tex, inv_vp, view,
                     current_camera_.position(),
                     math::Vector3f(0.6f, 0.6f, 0.7f), // fog 颜色
                     0.015f,                           // fog 密度
                     50.0f,                            // fog 高度
                     10.0f, 200.0f);                   // 近/远距离

        // 合成 fog 到场景颜色（使用 additive blending 避免 read-write 冲突）
        ctx_->set_framebuffer(fb_.color_fbo);
        ctx_->set_viewport(0, 0, viewport_width_, viewport_height_);
        ctx_->set_depth_test(false);
        ctx_->set_depth_write(false);
        ctx_->set_blend(true);
        ctx_->push_command([src = BlendFactor::One, dst = BlendFactor::SrcAlpha](IRenderBackend* backend) {
            backend->set_blend_func(src, dst);
        }); // additive blend
        fog_->render_apply(ctx_, fb_.color_tex, fb_.depth_tex, inv_vp,
                           current_camera_.position());
        ctx_->set_blend(false);
        ctx_->set_depth_test(true);
        ctx_->set_depth_write(true);
    }

    // 11.5 更新 SDFGI
    if (sdfgi_ && sdfgi_->valid()) {
        // 收集光源方向用于 SDFGI 光照注入
        std::vector<math::Vector3f> light_dirs;
        std::vector<math::Vector3f> light_colors;
        for (const auto& ld : scene_lights_) {
            light_dirs.push_back(ld.direction);
            light_colors.push_back(ld.color * ld.intensity);
        }
        sdfgi_->update(current_camera_.position(), light_dirs, light_colors);
    }

    // 12. 后处理
    _render_post_processing(ctx_);
}

// ---------------------------------------------------------------------------
// 子步骤实现
// ---------------------------------------------------------------------------

void RenderForwardClustered::_setup_cluster(const RenderData& data) {
    // 收集场景中的光源
    scene_lights_.clear();
    data.scene->foreach([&](scene::Entity* entity) {
        auto* light_comp = entity->get_component<components::Light>();
        if (!light_comp || !light_comp->enabled) return;

        LightData ld;
        ld.type = static_cast<LightType>(light_comp->light_type);
        // 位置从 Transform 获取
        auto* t = entity->get_component<components::Transform>();
        ld.position = t ? t->position : math::Vector3f::zero();
        ld.direction = light_comp->direction;
        ld.color = light_comp->color;
        ld.intensity = light_comp->intensity;
        ld.range = light_comp->range;
        ld.spot_angle = light_comp->spot_angle;
        ld.spot_softness = light_comp->spot_softness;
        ld.shadow_enabled = true;
        scene_lights_.push_back(ld);
    });

    // 构建集群
    cluster_builder_->build(current_camera_, scene_lights_,
                            viewport_width_, viewport_height_,
                            0.1f, 1000.0f);
}

void RenderForwardClustered::_render_shadows(RenderContext* ctx) {
    // 使用阴影系统渲染阴影贴图
    shadow_system_->render_shadows(ctx);

    // 渲染 CSM 级联阴影中的物体
    // 遍历不透明物体，对每个级联渲染深度
    for (int cascade = 0; cascade < shadow_system_->cascade_count(); ++cascade) {
        if (!shadow_system_->cascade_shadow_fbo(cascade).is_valid()) continue;

        const int size = shadow_system_->cascade_shadow_tex(cascade).is_valid() ?
            ctx->texture(shadow_system_->cascade_shadow_tex(cascade))->width() : 0;
        if (size <= 0) continue;

        ctx->set_framebuffer(shadow_system_->cascade_shadow_fbo(cascade));
        ctx->set_viewport(0, 0, size, size);
        ctx->clear_depth();
        ctx->set_depth_test(true);
        ctx->set_depth_write(true);
        ctx->set_cull_face(CullMode::Front);

        const math::Matrix4f light_mvp = shadow_system_->cascade_light_matrix(cascade);

        // 获取阴影渲染用的 shader
        // 使用基础变体 + 深度 prepass 的 shader 来渲染深度
        uint32_t shadow_variant = SHADER_VARIANT_DEPTH_NORMAL;
        RHIShaderHandle shadow_shader = shader_system_->get_shader(
            shadow_variant, SceneShaderForwardClustered::SHADER_GROUP_DEPTH_PREPASS);
        if (!shadow_shader.is_valid()) continue;

        for (const auto& elem : render_lists_[RENDER_LIST_OPAQUE]) {
            math::Matrix4f mvp = light_mvp * elem.transform;
            ctx->set_uniform_mat4(shadow_shader, "uModelMatrix", elem.transform);
            ctx->set_uniform_mat4(shadow_shader, "uMVP", mvp);
            ctx->draw_mesh(elem.mesh, shadow_shader);
        }

        ctx->set_cull_face(CullMode::None);
    }

    // 渲染聚光灯阴影
    for (size_t si = 0; si < shadow_system_->spot_shadow_count(); ++si) {
        if (!shadow_system_->spot_shadow_fbo(si).is_valid()) continue;

        const int size = 1024;

        ctx->set_framebuffer(shadow_system_->spot_shadow_fbo(si));
        ctx->set_viewport(0, 0, size, size);
        ctx->clear_depth();
        ctx->set_depth_test(true);
        ctx->set_depth_write(true);
        ctx->set_cull_face(CullMode::Front);

        // 获取阴影渲染 shader
        uint32_t shadow_variant = SHADER_VARIANT_DEPTH_NORMAL;
        RHIShaderHandle shadow_shader = shader_system_->get_shader(
            shadow_variant, SceneShaderForwardClustered::SHADER_GROUP_DEPTH_PREPASS);
        if (!shadow_shader.is_valid()) continue;

        // 获取聚光灯矩阵
        // 索引：CSM 级联之后
        const math::Matrix4f& spot_mvp = shadow_system_->spot_light_matrix(si);

        for (const auto& elem : render_lists_[RENDER_LIST_OPAQUE]) {
            math::Matrix4f mvp = spot_mvp * elem.transform;
            ctx->set_uniform_mat4(shadow_shader, "uModelMatrix", elem.transform);
            ctx->set_uniform_mat4(shadow_shader, "uMVP", mvp);
            ctx->draw_mesh(elem.mesh, shadow_shader);
        }

        ctx->set_cull_face(CullMode::None);
    }

    // 渲染点光源阴影（双抛物面映射）
    for (size_t pi = 0; pi < shadow_system_->point_shadow_count(); ++pi) {
        const int size = 512;
        const math::Vector3f& light_pos = shadow_system_->point_light_position(pi);

        // 获取阴影渲染 shader
        uint32_t shadow_variant = SHADER_VARIANT_DEPTH_NORMAL;
        RHIShaderHandle shadow_shader = shader_system_->get_shader(
            shadow_variant, SceneShaderForwardClustered::SHADER_GROUP_DEPTH_PREPASS);
        if (!shadow_shader.is_valid()) continue;

        for (int f = 0; f < 2; ++f) {
            if (!shadow_system_->point_shadow_fbo(pi, f).is_valid()) continue;

            ctx->set_framebuffer(shadow_system_->point_shadow_fbo(pi, f));
            ctx->set_viewport(0, 0, size, size);
            ctx->clear_depth();
            ctx->set_depth_test(true);
            ctx->set_depth_write(true);
            ctx->set_cull_face(CullMode::Front);

            const math::Matrix4f& light_view = shadow_system_->point_light_view_matrix(pi, f);
            float half_w = 20.0f; // 默认范围，从场景光源获取
            math::Matrix4f light_proj = math::Matrix4f::ortho(-half_w, half_w, -half_w, half_w, 0.01f, 40.0f);
            math::Matrix4f light_mvp = light_proj * light_view;

            for (const auto& elem : render_lists_[RENDER_LIST_OPAQUE]) {
                math::Matrix4f mvp = light_mvp * elem.transform;
                ctx->set_uniform_mat4(shadow_shader, "uModelMatrix", elem.transform);
                ctx->set_uniform_mat4(shadow_shader, "uMVP", mvp);
                ctx->draw_mesh(elem.mesh, shadow_shader);
            }

            ctx->set_cull_face(CullMode::None);
        }
    }
}

void RenderForwardClustered::_render_depth_prepass(RenderContext* ctx) {
    // Depth Prepass：渲染不透明物体的深度，可选法线+粗糙度
    // 提高后续 pass 的渲染效率（early-z rejection）
    if (!fb_.depth_normal_fbo.is_valid()) return;

    ctx->set_framebuffer(fb_.depth_normal_fbo);
    ctx->set_viewport(0, 0, viewport_width_, viewport_height_);
    ctx->clear_depth();
    ctx->set_depth_test(true);
    ctx->set_depth_write(true);
    ctx->set_blend(false);
    ctx->set_cull_face(CullMode::Back);

    for (const auto& elem : render_lists_[RENDER_LIST_OPAQUE]) {
        uint32_t variant = elem.variant_key | SHADER_VARIANT_DEPTH_NORMAL;
        RHIShaderHandle shader = shader_system_->get_shader(
            variant, SceneShaderForwardClustered::SHADER_GROUP_DEPTH_PREPASS);
        if (!shader.is_valid()) continue;

        ctx->set_uniform_mat4(shader, "uModelMatrix", elem.transform);
        ctx->draw_mesh(elem.mesh, shader);
    }
}

void RenderForwardClustered::_render_motion_vectors(RenderContext* ctx) {
    // 运动向量 pass：渲染物体从上一帧到当前帧的屏幕空间运动
    if (!fb_.motion_vectors_fbo.is_valid()) return;

    ctx->set_framebuffer(fb_.motion_vectors_fbo);
    ctx->set_viewport(0, 0, viewport_width_, viewport_height_);
    ctx->clear(0.0f, 0.0f, 0.0f, 0.0f);
    ctx->clear_depth();
    ctx->set_depth_test(true);
    ctx->set_depth_write(true);
    ctx->set_blend(false);
    ctx->set_cull_face(CullMode::Back);

    const math::Matrix4f view_proj = current_camera_.get_projection_matrix() * current_camera_.get_view_matrix();

    for (const auto& elem : render_lists_[RENDER_LIST_MOTION]) {
        uint32_t variant = elem.variant_key | SHADER_VARIANT_MOTION_VECTORS;
        RHIShaderHandle shader = shader_system_->get_shader(
            variant, SceneShaderForwardClustered::SHADER_GROUP_MOTION_VECTORS);
        if (!shader.is_valid()) continue;

        math::Matrix4f mvp = view_proj * elem.transform;
        math::Matrix4f prev_mvp = view_proj * elem.prev_transform;
        ctx->set_uniform_mat4(shader, "uModelMatrix", elem.transform);
        ctx->set_uniform_mat4(shader, "uMVP", mvp);
        ctx->set_uniform_mat4(shader, "uPrevMVP", prev_mvp);
        ctx->draw_mesh(elem.mesh, shader);
    }
}

void RenderForwardClustered::_render_opaque_pass(RenderContext* ctx) {
    ctx->set_viewport(0, 0, viewport_width_, viewport_height_);
    ctx->set_depth_test(true);
    ctx->set_depth_write(true);
    ctx->set_blend(false);
    ctx->set_cull_face(CullMode::Back);

    const math::Matrix4f view = current_camera_.get_view_matrix();
    const math::Matrix4f proj = current_camera_.get_projection_matrix();
    const math::Matrix4f view_proj = proj * view;

    // ---- 绑定阴影贴图纹理（全局 bind）----
    // 使用预先定义的 slot 常量，避免与 PBR 材质纹理冲突
    // CSM 阴影 slot: 15, 17, 18, 19 (复用 kPBRShadow 体系)
    static constexpr int kCSMSlots[4] = {
        TextureSlots::kPBRShadow,      // 15
        TextureSlots::kPBRShadowC1,    // 17
        TextureSlots::kPBRShadowC2,    // 18
        TextureSlots::kPBRShadowC3     // 19
    };
    // CSM 原始深度 slot: 25..28
    static constexpr int kCSMDepthSlots[4] = {
        TextureSlots::kPBRShadowDepth,
        TextureSlots::kPBRShadowDepth1,
        TextureSlots::kPBRShadowDepth2,
        TextureSlots::kPBRShadowDepth3
    };
    // 聚光灯阴影 slot: 0..3（与 2D 不重叠，2D 使用 slot 0/1 但不在 3D pass 中）
    static constexpr int kSpotSlots[4] = {0, 1, 2, 3};
    // SSAO slot
    static constexpr int kSSAOSlot = TextureSlots::kPBRSSAO; // 33

    // 绑定 CSM 阴影贴图
    int cascade_count = std::min(shadow_system_->cascade_count(), 4);
    for (int c = 0; c < cascade_count; ++c) {
        RHITextureHandle shadow_tex = shadow_system_->cascade_shadow_tex(c);
        if (shadow_tex.is_valid()) {
            ctx->set_texture_raw_depth({}, shadow_tex, kCSMSlots[c], nullptr);
            // 也绑定原始深度 sampler（PCSS 需要）
            ctx->set_texture_raw_depth({}, shadow_tex, kCSMDepthSlots[c], nullptr);
        }
    }

    // 绑定聚光灯阴影贴图
    int spot_count = std::min(static_cast<int>(shadow_system_->spot_shadow_count()), 4);
    for (int s = 0; s < spot_count; ++s) {
        RHITextureHandle spot_tex = shadow_system_->spot_shadow_tex(s);
        if (spot_tex.is_valid()) {
            ctx->set_texture_raw_depth({}, spot_tex, kSpotSlots[s], nullptr);
        }
    }

    // 绑定 SSAO 纹理
    if (ssao_targets_valid_ && ssao_tex_[1].is_valid()) {
        ctx->set_texture({}, ssao_tex_[1], kSSAOSlot, nullptr);
    }

    // 环境光（从 RenderData 传入）
    math::Vector3f ambient = ambient_light_;

    // 绑定光源 SSBO（如果存在）— 供支持 SSBO 的 shader 使用
    if (light_buffer_.is_valid()) {
        IBuffer* buf = ctx->buffer(light_buffer_);
        if (buf) {
            buf->bind(0); // binding point 0
        }
    }

    for (const auto& elem : render_lists_[RENDER_LIST_OPAQUE]) {
        RHIShaderHandle shader = shader_system_->get_shader(
            elem.variant_key, SceneShaderForwardClustered::SHADER_GROUP_BASE);
        if (!shader.is_valid()) continue;

        math::Matrix4f mvp = view_proj * elem.transform;
        math::Matrix4f normal_matrix = elem.transform.inverse().transpose();

        ctx->set_uniform_mat4(shader, "uModelMatrix", elem.transform);
        ctx->set_uniform_mat4(shader, "uViewMatrix", view);
        ctx->set_uniform_mat4(shader, "uProjectionMatrix", proj);
        ctx->set_uniform_mat4(shader, "uMVP", mvp);
        ctx->set_uniform_mat4(shader, "uNormalMatrix", normal_matrix);
        ctx->set_uniform_vec3(shader, "uCameraPos", current_camera_.position());
        ctx->set_uniform_vec3(shader, "uAmbient", ambient);

        // ---- 阴影 uniform ----
        ctx->set_uniform_int(shader, "uCascadeCount", cascade_count);
        ctx->set_uniform_int(shader, "uPCSSEnabled", 0);
        ctx->set_uniform_float(shader, "uPCSSLightSize", 0.005f);
        ctx->set_uniform_float(shader, "uPCSSMaxRadius", 20.0f);
        ctx->set_uniform_float(shader, "uPCSSBlockerScale", 1.0f);

        if (cascade_count > 0) {
            ctx->set_uniform_vec4(shader, "uCascadeSplits",
                math::Vector4f(
                    shadow_system_->cascade_split(0),
                    shadow_system_->cascade_split(1),
                    shadow_system_->cascade_split(2),
                    shadow_system_->cascade_split(3)));
            ctx->set_uniform_vec4(shader, "uCascadeFarBlend",
                math::Vector4f(shadow_system_->cascade_split(4), 0.15f, 0.0f, 0.0f));
            ctx->set_uniform_vec4(shader, "uCascadeBias",
                math::Vector4f(0.0005f, 0.001f, 0.002f, 0.004f));

            for (int c = 0; c < cascade_count; ++c) {
                std::string mat_name = "uCascadeLightSpace[" + std::to_string(c) + "]";
                ctx->set_uniform_mat4(shader, mat_name.c_str(), shadow_system_->cascade_light_matrix(c));
            }
        }

        // 聚光灯阴影
        for (int s = 0; s < spot_count; ++s) {
            std::string mat_name = "uSpotLightSpace[" + std::to_string(s) + "]";
            ctx->set_uniform_mat4(shader, mat_name.c_str(), shadow_system_->spot_light_matrix(s));
        }

        // 阴影 sampler 绑定到 slot
        for (int c = 0; c < cascade_count; ++c) {
            const char* tex_name = (c == 0) ? "uShadowMap" :
                (c == 1) ? "uShadowMap1" :
                (c == 2) ? "uShadowMap2" : "uShadowMap3";
            ctx->set_uniform_int(shader, tex_name, kCSMSlots[c]);
            // 原始深度 sampler
            const char* depth_name = (c == 0) ? "uShadowMapDepth" :
                (c == 1) ? "uShadowMapDepth1" :
                (c == 2) ? "uShadowMapDepth2" : "uShadowMapDepth3";
            ctx->set_uniform_int(shader, depth_name, kCSMDepthSlots[c]);
        }
        for (int s = 0; s < spot_count; ++s) {
            std::string tex_name = "uSpotShadowMap" + std::to_string(s);
            ctx->set_uniform_int(shader, tex_name.c_str(), kSpotSlots[s]);
        }

        // SSAO
        ctx->set_uniform_int(shader, "uUseSSAO", pp_params_.ssao_enabled ? 1 : 0);
        ctx->set_uniform_float(shader, "uSSAOStrength", 1.0f);
        ctx->set_uniform_int(shader, "uSSAOTexture", kSSAOSlot);

        // 光照 uniform
        _set_light_uniforms(ctx, shader);

        ctx->draw_mesh(elem.mesh, shader);
    }

    ctx->set_depth_write(false);
}

void RenderForwardClustered::_render_sky(RenderContext* ctx) {
    if (!skybox_initialized_ || !skybox_shader_.is_valid()) return;

    RHIShaderHandle shader = skybox_shader_;
    if (!shader.is_valid()) return;

    // 天空盒在 opaque pass 之后渲染到主 color buffer
    ctx->set_framebuffer(fb_.color_fbo);
    ctx->set_viewport(0, 0, viewport_width_, viewport_height_);
    ctx->set_depth_test(false);
    ctx->set_depth_write(false);
    ctx->set_blend(false);
    ctx->set_cull_face(CullMode::None);

    // 计算逆 VP 矩阵
    math::Matrix4f view = current_camera_.get_view_matrix();
    math::Matrix4f proj = current_camera_.get_projection_matrix();
    math::Matrix4f inv_vp = (proj * view).inverse();

    ctx->set_uniform_mat4(shader, "uInvViewProj", inv_vp);
    ctx->set_uniform_mat4(shader, "uViewMatrix", view);

    // 天空盒颜色参数
    ctx->set_uniform_vec4(shader, "uSkyColor0",
        math::Vector4f(sky_color0_.x, sky_color0_.y, sky_color0_.z, 1.0f));
    ctx->set_uniform_vec4(shader, "uSkyColor1",
        math::Vector4f(sky_color1_.x, sky_color1_.y, sky_color1_.z, 1.0f));
    ctx->set_uniform_vec4(shader, "uSkyColor2",
        math::Vector4f(sky_color2_.x, sky_color2_.y, sky_color2_.z, 1.0f));
    ctx->set_uniform_float(shader, "uSunIntensity", sky_sun_intensity_);
    ctx->set_uniform_vec3(shader, "uSunDirection", sky_sun_direction_);
    ctx->set_uniform_int(shader, "uUseTexturedSky", sky_use_texture_ ? 1 : 0);

    // 绑定 cubemap 纹理（如果存在）
    if (sky_use_texture_ && skybox_tex_.is_valid()) {
        ctx->set_uniform_int(shader, "uSkyboxTex", TextureSlots::kSkyboxCube);
        ctx->set_texture({}, skybox_tex_, TextureSlots::kSkyboxCube, nullptr);
    }

    // 使用全屏 mesh 或绘制全屏三角
    if (fullscreen_mesh_.is_valid()) {
        ctx->draw_mesh(fullscreen_mesh_, shader);
    }

    ctx->set_depth_test(true);
    ctx->set_depth_write(true);
}

void RenderForwardClustered::_render_alpha_pass(RenderContext* ctx) {
    ctx->set_viewport(0, 0, viewport_width_, viewport_height_);
    ctx->set_depth_test(true);
    ctx->set_depth_write(false);
    ctx->set_blend(true);
    ctx->set_cull_face(CullMode::None);

    const math::Matrix4f view = current_camera_.get_view_matrix();
    const math::Matrix4f proj = current_camera_.get_projection_matrix();
    const math::Matrix4f view_proj = proj * view;

    for (const auto& elem : render_lists_[RENDER_LIST_ALPHA]) {
        uint32_t variant = elem.variant_key | SHADER_VARIANT_TRANSPARENT;
        RHIShaderHandle shader = shader_system_->get_shader(
            variant, SceneShaderForwardClustered::SHADER_GROUP_BASE);
        if (!shader.is_valid()) continue;

        math::Matrix4f mvp = view_proj * elem.transform;
        math::Matrix4f normal_matrix = elem.transform.inverse().transpose();

        ctx->set_uniform_mat4(shader, "uModelMatrix", elem.transform);
        ctx->set_uniform_mat4(shader, "uViewMatrix", view);
        ctx->set_uniform_mat4(shader, "uProjectionMatrix", proj);
        ctx->set_uniform_mat4(shader, "uMVP", mvp);
        ctx->set_uniform_mat4(shader, "uNormalMatrix", normal_matrix);
        ctx->set_uniform_vec3(shader, "uCameraPos", current_camera_.position());

        _set_light_uniforms(ctx, shader);
        ctx->draw_mesh(elem.mesh, shader);
    }

    ctx->set_blend(false);
    ctx->set_depth_write(true);
}

void RenderForwardClustered::_render_decals(RenderContext* ctx) {
    if (!decal_initialized_ || !decal_shader_.is_valid() || !decal_box_mesh_.is_valid()) return;
    if (!decal_storage_ || decal_storage_->decal_count() == 0) return;

    // 贴花渲染：使用 depth buffer 重建世界位置，投影到场景几何体表面
    // 需要：depth texture, inverse view-projection, camera matrices
    ctx->set_framebuffer(fb_.color_fbo);
    ctx->set_viewport(0, 0, viewport_width_, viewport_height_);
    ctx->set_depth_test(true);
    ctx->set_depth_write(false);
    ctx->set_blend(true);
    ctx->push_command([src = BlendFactor::SrcAlpha, dst = BlendFactor::OneMinusSrcAlpha](IRenderBackend* backend) {
        backend->set_blend_func(src, dst);
    });
    ctx->set_cull_face(CullMode::None);

    const math::Matrix4f view = current_camera_.get_view_matrix();
    const math::Matrix4f proj = current_camera_.get_projection_matrix();
    const math::Matrix4f inv_vp = (proj * view).inverse();

    RHIShaderHandle shader = decal_shader_;

    // 绑定通用的 depth texture 和相机矩阵
    ctx->set_texture_raw_depth(shader, fb_.depth_tex, TextureSlots::kPBRShadow, "uDepthTex");
    ctx->set_uniform_int(shader, "uDepthTex", TextureSlots::kPBRShadow);
    ctx->set_uniform_mat4(shader, "uInvViewProj", inv_vp);
    ctx->set_uniform_mat4(shader, "uViewMatrix", view);
    ctx->set_uniform_mat4(shader, "uProjectionMatrix", proj);
    ctx->push_command([shader, w = static_cast<float>(viewport_width_), h = static_cast<float>(viewport_height_)](IRenderBackend* backend) {
        IShader* s = backend->shader(shader);
        if (s) s->set_vec2("uScreenSize", math::Vector2f(w, h));
    });

    for (int i = 0; i < decal_storage_->decal_count(); ++i) {
        const DecalData& decal = decal_storage_->get_decal(i);
        if (!decal.enabled) continue;

        // 构建模型矩阵：平移 * 旋转(欧拉角) * 缩放
        math::Matrix4f rot_x = math::Matrix4f::rotate(math::to_radians(decal.rotation.x), math::Vector3f(1, 0, 0));
        math::Matrix4f rot_y = math::Matrix4f::rotate(math::to_radians(decal.rotation.y), math::Vector3f(0, 1, 0));
        math::Matrix4f rot_z = math::Matrix4f::rotate(math::to_radians(decal.rotation.z), math::Vector3f(0, 0, 1));
        math::Matrix4f model = math::Matrix4f::translate(decal.position) * rot_z * rot_y * rot_x * math::Matrix4f::scale(decal.scale);

        // uWorldToDecal = inverse(model)
        math::Matrix4f world_to_decal = model.inverse();

        ctx->set_uniform_mat4(shader, "uModelMatrix", model);
        ctx->set_uniform_mat4(shader, "uWorldToDecal", world_to_decal);
        ctx->set_uniform_vec3(shader, "uDecalAlbedo", decal.albedo);
        ctx->set_uniform_float(shader, "uDecalOpacity", decal.opacity);
        ctx->set_uniform_int(shader, "uUseAlbedoTex", 0); // 暂不支持纹理贴花

        ctx->draw_mesh(decal_box_mesh_, shader);
    }

    ctx->set_blend(false);
    ctx->set_cull_face(CullMode::Back);
    ctx->set_depth_write(true);
}

void RenderForwardClustered::_render_post_processing(RenderContext* ctx) {
    // 后处理链（与 Godot 顺序一致）
    // 0. SSS（次表面散射：深度加权可分离模糊）
    if (pp_params_.sss_enabled && subsurface_scattering_ && subsurface_scattering_->valid()) {
        subsurface_scattering_->render(ctx, fb_.color_tex, fb_.depth_tex, fb_.color_fbo,
                                       pp_params_, viewport_width_, viewport_height_);
    }

    // 1. SSAO（半分辨率，从深度重建 AO）
    _render_ssao(ctx);

    // 2. SSR（屏幕空间反射，使用 HiZ 加速）
    if (pp_params_.ssr_enabled) {
        ssr_->render(ctx, fb_.color_tex, fb_.depth_tex, fb_.depth_normal_tex,
                     pp_params_, viewport_width_, viewport_height_);
    }

    // 3. SSIL（屏幕空间间接光照）
    if (pp_params_.ssil_enabled) {
        ssil_->render(ctx, fb_.color_tex, fb_.depth_tex, fb_.depth_normal_tex,
                      pp_params_, viewport_width_, viewport_height_);
    }

    // 4. Bloom（阈值提取 → 降采样 → 上采样合成）
    _render_bloom(ctx);

    // 5. Bokeh DOF（景深）
    if (pp_params_.dof_enabled) {
        bokeh_dof_->render(ctx, fb_.color_tex, fb_.depth_tex,
                           pp_params_, viewport_width_, viewport_height_);
    }

    // 6. 自动曝光（亮度链 → 1x1 → 曝光更新）
    _render_auto_exposure(ctx);

    // 7. FSR2（超分辨率上采样）
    if (pp_params_.fsr2_enabled && fsr2_->valid()) {
        float jx, jy;
        FSR2_RD::get_jitter(frame_index_,
                            pp_params_.fsr2_render_width > 0 ? pp_params_.fsr2_render_width : viewport_width_,
                            pp_params_.fsr2_render_height > 0 ? pp_params_.fsr2_render_height : viewport_height_,
                            jx, jy);
        RHITextureHandle exp_tex = (exposure_tex_[current_exposure_idx_].is_valid() && auto_exposure_targets_valid_)
            ? exposure_tex_[current_exposure_idx_] : fb_.color_tex;
        fsr2_->render(ctx, fb_.color_tex, fb_.depth_tex, fb_.motion_vectors_tex, exp_tex,
                      pp_params_, jx, jy);
    }

    // 8. TAA（时域累积 + 半像素抖动 + 邻域钳制）
    _render_taa(ctx);

    // 9. ToneMap（HDR → LDR，含 Bloom 合成 + 曝光 + LUT）
    _render_tonemap(ctx);
}

// ---------------------------------------------------------------------------
// 渲染列表管理
// ---------------------------------------------------------------------------

void RenderForwardClustered::clear_render_lists() {
    for (int i = 0; i < RENDER_LIST_MAX; ++i) {
        render_lists_[i].clear();
    }
}

void RenderForwardClustered::add_to_render_list(RenderListType list, const RenderElement& element) {
    render_lists_[list].push_back(element);
}

void RenderForwardClustered::sort_render_list(RenderListType list) {
    auto& elems = render_lists_[list];
    if (list == RENDER_LIST_ALPHA) {
        // 透明物体：从远到近（distance_sq 降序）
        std::sort(elems.begin(), elems.end(), [](const RenderElement& a, const RenderElement& b) {
            return a.distance_sq > b.distance_sq;
        });
    } else {
        // 不透明物体：按 sort_key 升序（减少 shader 切换）
        std::sort(elems.begin(), elems.end(), [](const RenderElement& a, const RenderElement& b) {
            return a.sort_key < b.sort_key;
        });
    }
}

const std::vector<RenderForwardClustered::RenderElement>& RenderForwardClustered::get_render_list(RenderListType list) const {
    return render_lists_[list];
}

// ---------------------------------------------------------------------------
// 渲染列表填充
// ---------------------------------------------------------------------------

void RenderForwardClustered::_populate_render_lists(const RenderData& data) {
    clear_render_lists();

    data.scene->foreach([&](scene::Entity* entity) {
        auto* t = entity->get_component<components::Transform>();
        if (!t) return;

        // 计算世界矩阵（简化：使用本地矩阵，TODO: 实际应从父级传播）
        math::Matrix4f world = t->local_matrix();
        math::Matrix4f prev_world = world; // TODO: 存储上一帧变换
        math::Vector3f world_pos = t->position;

        // 收集 MeshRenderer
        auto* mesh_renderer = entity->get_component<components::MeshRenderer>();
        if (mesh_renderer && mesh_renderer->enabled) {
            RenderElement elem;
            elem.mesh = mesh_renderer->gpu_mesh_handle();
            elem.transform = world;
            elem.prev_transform = prev_world;
            elem.variant_key = _compute_variant_key(entity, false);
            elem.skinned = false;

            // 到相机的距离
            math::Vector3f cam_pos = current_camera_.position();
            elem.distance_sq = (world_pos - cam_pos).length_sq();

            // 判断是否透明
            bool transparent = mesh_renderer->material &&
                mesh_renderer->material->blend_mode == render::Material::BlendMode::Blend;

            if (transparent) {
                add_to_render_list(RENDER_LIST_ALPHA, elem);
            } else {
                add_to_render_list(RENDER_LIST_OPAQUE, elem);
                add_to_render_list(RENDER_LIST_MOTION, elem);
            }
        }

        // 收集 SkinnedMeshRenderer
        auto* skinned_renderer = entity->get_component<components::SkinnedMeshRenderer>();
        if (skinned_renderer && skinned_renderer->enabled) {
            RenderElement elem;
            elem.mesh = skinned_renderer->gpu_mesh_handle();
            elem.transform = world;
            elem.prev_transform = prev_world;
            elem.variant_key = _compute_variant_key(entity, false) | SHADER_VARIANT_SKINNED;
            elem.skinned = true;

            math::Vector3f cam_pos = current_camera_.position();
            elem.distance_sq = (world_pos - cam_pos).length_sq();

            bool transparent = skinned_renderer->material &&
                skinned_renderer->material->blend_mode == render::Material::BlendMode::Blend;

            if (transparent) {
                add_to_render_list(RENDER_LIST_ALPHA, elem);
            } else {
                add_to_render_list(RENDER_LIST_OPAQUE, elem);
                add_to_render_list(RENDER_LIST_MOTION, elem);
            }
        }
    });

    // 排序不透明列表
    sort_render_list(RENDER_LIST_OPAQUE);

    // 收集贴花数据
    if (decal_storage_) {
        decal_storage_->clear();
        ecs::foreach_with_component<components::Decal>(*data.scene,
            [&](scene::Entity* entity, components::Decal* decal_comp) {
                if (!decal_comp->enabled) return;
                auto* t = entity->get_component<components::Transform>();
                if (!t) return;

                DecalData dd;
                dd.position = t->position;
                dd.rotation = t->rotation.to_euler(); // 欧拉角
                dd.scale = decal_comp->size;
                dd.albedo = decal_comp->color;
                dd.roughness = decal_comp->roughness;
                dd.metallic = decal_comp->metallic;
                dd.opacity = decal_comp->albedo_blend;
                dd.enabled = true;
                decal_storage_->add_decal(dd);
            });
    }
}

// ---------------------------------------------------------------------------
// 辅助方法
// ---------------------------------------------------------------------------

uint32_t RenderForwardClustered::_compute_variant_key(const scene::Entity* entity, bool transparent) const {
    uint32_t key = SHADER_VARIANT_BASE;
    if (transparent) key |= SHADER_VARIANT_TRANSPARENT;
    // TODO: 根据 entity 的材质/组件计算更多变体标志
    return key;
}

void RenderForwardClustered::_set_light_uniforms(RenderContext* ctx, RHIShaderHandle shader) {
    // SSBO 路径：光源数据通过 SSBO 传递，只需设置光源计数和阴影索引
    const auto& culled_lights = cluster_builder_->culled_lights();
    const int light_count = std::min(static_cast<int>(culled_lights.size()), 256);

    // 找到方向光阴影索引（第一个有阴影的方向光）
    int shadow_light_index = -1;
    int spot_shadow_count = 0;

    for (int i = 0; i < light_count; ++i) {
        const auto& light = culled_lights[i];
        if (light.type == LightType::Directional && light.shadow_enabled && shadow_light_index < 0) {
            shadow_light_index = i;
        }
        if (light.type == LightType::Spot && light.shadow_enabled) {
            spot_shadow_count++;
        }
    }

    ctx->set_uniform_int(shader, "uLightCount", light_count);
    ctx->set_uniform_int(shader, "uShadowLightIndex", shadow_light_index);
    ctx->set_uniform_int(shader, "uUseShadowMap", shadow_light_index >= 0 ? 1 : 0);
    ctx->set_uniform_int(shader, "uSpotShadowCount", spot_shadow_count);
}

// ---------------------------------------------------------------------------
// 内部 framebuffer 创建
// ---------------------------------------------------------------------------

bool RenderForwardClustered::_create_internal_framebuffers(int width, int height) {
    if (width <= 0 || height <= 0) return false;

    // 如果大小相同，跳过重建
    if (fb_.width == width && fb_.height == height && fb_.color_fbo.is_valid()) {
        return true;
    }

    // 销毁旧的 framebuffer 和纹理
    if (fb_.color_tex.is_valid()) { ctx_->destroy_texture(fb_.color_tex); fb_.color_tex = {}; }
    if (fb_.depth_tex.is_valid()) { ctx_->destroy_texture(fb_.depth_tex); fb_.depth_tex = {}; }
    if (fb_.depth_normal_tex.is_valid()) { ctx_->destroy_texture(fb_.depth_normal_tex); fb_.depth_normal_tex = {}; }
    if (fb_.motion_vectors_tex.is_valid()) { ctx_->destroy_texture(fb_.motion_vectors_tex); fb_.motion_vectors_tex = {}; }
    if (fb_.viewport_tex.is_valid()) { ctx_->destroy_texture(fb_.viewport_tex); fb_.viewport_tex = {}; }

    if (fb_.color_fbo.is_valid()) { ctx_->destroy_framebuffer(fb_.color_fbo); fb_.color_fbo = {}; }
    if (fb_.depth_fbo.is_valid()) { ctx_->destroy_framebuffer(fb_.depth_fbo); fb_.depth_fbo = {}; }
    if (fb_.depth_normal_fbo.is_valid()) { ctx_->destroy_framebuffer(fb_.depth_normal_fbo); fb_.depth_normal_fbo = {}; }
    if (fb_.motion_vectors_fbo.is_valid()) { ctx_->destroy_framebuffer(fb_.motion_vectors_fbo); fb_.motion_vectors_fbo = {}; }
    if (fb_.viewport_fbo.is_valid()) { ctx_->destroy_framebuffer(fb_.viewport_fbo); fb_.viewport_fbo = {}; }

    fb_.width = width;
    fb_.height = height;

    // --- 1. 主颜色 RT（RGBA16F HDR）---
    {
        fb_.color_tex = ctx_->create_texture();
        ITexture* tex = ctx_->texture(fb_.color_tex);
        if (!fb_.color_tex.is_valid() || !tex ||
            !tex->create(TextureFormat::RGBA16F, width, height, nullptr)) {
            GLOG_ERROR("RenderForwardClustered: failed to create color texture");
            return false;
        }
        tex->set_filter(TextureFilter::Linear, TextureFilter::Linear);
        tex->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

        fb_.color_fbo = ctx_->create_framebuffer();
        IFramebuffer* fbo = ctx_->framebuffer(fb_.color_fbo);
        if (!fb_.color_fbo.is_valid() || !fbo || !fbo->create(width, height)) return false;
        fbo->attach_color_texture(tex);
        if (!fbo->is_complete()) {
            GLOG_ERROR("RenderForwardClustered: color FBO incomplete");
            return false;
        }
    }

    // --- 2. 深度 RT（Depth24）---
    {
        fb_.depth_tex = ctx_->create_texture();
        ITexture* tex = ctx_->texture(fb_.depth_tex);
        if (!fb_.depth_tex.is_valid() || !tex ||
            !tex->create(TextureFormat::Depth24, width, height, nullptr)) {
            GLOG_ERROR("RenderForwardClustered: failed to create depth texture");
            return false;
        }
        tex->set_filter(TextureFilter::Nearest, TextureFilter::Nearest);
        tex->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

        fb_.depth_fbo = ctx_->create_framebuffer();
        IFramebuffer* fbo = ctx_->framebuffer(fb_.depth_fbo);
        if (!fb_.depth_fbo.is_valid() || !fbo || !fbo->create(width, height)) return false;
        fbo->attach_depth_texture(tex);
        if (!fbo->is_complete()) {
            GLOG_ERROR("RenderForwardClustered: depth FBO incomplete");
            return false;
        }
    }

    // --- 3. 深度+法线+粗糙度 RT（Depth Prepass 输出）---
    {
        // 使用 RGBA16F 存储法线 (xyz) + 粗糙度 (w)
        fb_.depth_normal_tex = ctx_->create_texture();
        ITexture* tex = ctx_->texture(fb_.depth_normal_tex);
        if (!fb_.depth_normal_tex.is_valid() || !tex ||
            !tex->create(TextureFormat::RGBA16F, width, height, nullptr)) {
            GLOG_ERROR("RenderForwardClustered: failed to create depth_normal texture");
            return false;
        }
        tex->set_filter(TextureFilter::Nearest, TextureFilter::Nearest);
        tex->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

        fb_.depth_normal_fbo = ctx_->create_framebuffer();
        IFramebuffer* fbo = ctx_->framebuffer(fb_.depth_normal_fbo);
        if (!fb_.depth_normal_fbo.is_valid() || !fbo || !fbo->create(width, height)) return false;
        fbo->attach_color_texture(tex);
        // 同时 attach 深度纹理作为深度缓冲
        ITexture* depth_tex = ctx_->texture(fb_.depth_tex);
        if (depth_tex) fbo->attach_depth_texture(depth_tex);
        if (!fbo->is_complete()) {
            GLOG_ERROR("RenderForwardClustered: depth_normal FBO incomplete");
            return false;
        }
    }

    // --- 4. 运动向量 RT（RGBA16F，实际只使用 xy 分量）---
    {
        fb_.motion_vectors_tex = ctx_->create_texture();
        ITexture* tex = ctx_->texture(fb_.motion_vectors_tex);
        if (!fb_.motion_vectors_tex.is_valid() || !tex ||
            !tex->create(TextureFormat::RGBA16F, width, height, nullptr)) {
            GLOG_ERROR("RenderForwardClustered: failed to create motion_vectors texture");
            return false;
        }
        tex->set_filter(TextureFilter::Nearest, TextureFilter::Nearest);
        tex->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

        fb_.motion_vectors_fbo = ctx_->create_framebuffer();
        IFramebuffer* fbo = ctx_->framebuffer(fb_.motion_vectors_fbo);
        if (!fb_.motion_vectors_fbo.is_valid() || !fbo || !fbo->create(width, height)) return false;
        fbo->attach_color_texture(tex);
        ITexture* depth_tex = ctx_->texture(fb_.depth_tex);
        if (depth_tex) fbo->attach_depth_texture(depth_tex);
        if (!fbo->is_complete()) {
            GLOG_ERROR("RenderForwardClustered: motion_vectors FBO incomplete");
            return false;
        }
    }

    // --- 5. 视口输出 RT（RGBA8，编辑器离屏渲染）---
    {
        fb_.viewport_tex = ctx_->create_texture();
        ITexture* tex = ctx_->texture(fb_.viewport_tex);
        if (!fb_.viewport_tex.is_valid() || !tex ||
            !tex->create(TextureFormat::RGBA8, width, height, nullptr)) {
            GLOG_ERROR("RenderForwardClustered: failed to create viewport texture");
            return false;
        }
        tex->set_filter(TextureFilter::Linear, TextureFilter::Linear);
        tex->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

        fb_.viewport_fbo = ctx_->create_framebuffer();
        IFramebuffer* fbo = ctx_->framebuffer(fb_.viewport_fbo);
        if (!fb_.viewport_fbo.is_valid() || !fbo || !fbo->create(width, height)) return false;
        fbo->attach_color_texture(tex);
        if (!fbo->is_complete()) {
            GLOG_ERROR("RenderForwardClustered: viewport FBO incomplete");
            return false;
        }
        viewport_color_tex_ = tex;
    }

    // 重建后处理 target
    _destroy_bloom_targets();
    _destroy_ssao_targets();
    _destroy_taa_targets();
    _destroy_auto_exposure_targets();
    _destroy_sss_targets();

    _create_bloom_targets();
    _create_ssao_targets();
    _create_taa_targets();
    _create_auto_exposure_targets();
    _create_sss_targets();

    // 重建 SSR/SSIL/Bokeh DOF/FSR2 目标
    ssr_->create_hiz(width, height);
    ssil_->create_targets(width, height);
    bokeh_dof_->create_targets(width, height);
    if (pp_params_.fsr2_enabled) {
        fsr2_->create_targets(width, height, width, height);
    }

    return true;
}

// ---------------------------------------------------------------------------
// 视口接口
// ---------------------------------------------------------------------------

void RenderForwardClustered::set_viewport_output_enabled(bool enabled) {
    viewport_output_enabled_ = enabled;
}

bool RenderForwardClustered::viewport_output_enabled() const {
    return viewport_output_enabled_;
}

ITexture* RenderForwardClustered::viewport_color_texture() const {
    return viewport_color_tex_;
}

bool RenderForwardClustered::resize_render_targets(int width, int height) {
    bool result = _create_internal_framebuffers(width, height);
    if (fog_) {
        fog_->create_targets(width, height);
    }
    return result;
}

bool RenderForwardClustered::hot_reload() {
    return shader_system_->hot_reload();
}

int RenderForwardClustered::poll_shader_hot_reload(RenderContext& ctx) {
    return shader_system_->poll_hot_reload(ctx);
}

// ---------------------------------------------------------------------------
// 后处理 Target 创建/销毁
// ---------------------------------------------------------------------------

bool RenderForwardClustered::_create_bloom_targets() {
    // 半分辨率链：L0 = w/2 ... L4 = w/32（最小 16）
    int w = std::max(16, viewport_width_ / 2);
    int h = std::max(16, viewport_height_ / 2);
    for (int i = 0; i < k_bloom_levels; ++i) {
        bloom_level_w_[i] = w;
        bloom_level_h_[i] = h;

        bloom_down_tex_[i] = ctx_->create_texture();
        ITexture* down_tex = ctx_->texture(bloom_down_tex_[i]);
        if (!bloom_down_tex_[i].is_valid() || !down_tex ||
            !down_tex->create(TextureFormat::RGBA16F, w, h, nullptr)) {
            GLOG_ERROR("RenderForwardClustered: bloom down texture {} failed ({}x{})", i, w, h);
            return false;
        }
        down_tex->set_filter(TextureFilter::Linear, TextureFilter::Linear);
        down_tex->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

        bloom_down_fbo_[i] = ctx_->create_framebuffer();
        IFramebuffer* down_fbo = ctx_->framebuffer(bloom_down_fbo_[i]);
        if (!bloom_down_fbo_[i].is_valid() || !down_fbo || !down_fbo->create(w, h)) return false;
        down_fbo->attach_color_texture(down_tex);
        if (!down_fbo->is_complete()) return false;

        bloom_up_tex_[i] = ctx_->create_texture();
        ITexture* up_tex = ctx_->texture(bloom_up_tex_[i]);
        if (!bloom_up_tex_[i].is_valid() || !up_tex ||
            !up_tex->create(TextureFormat::RGBA16F, w, h, nullptr)) {
            GLOG_ERROR("RenderForwardClustered: bloom up texture {} failed ({}x{})", i, w, h);
            return false;
        }
        up_tex->set_filter(TextureFilter::Linear, TextureFilter::Linear);
        up_tex->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

        bloom_up_fbo_[i] = ctx_->create_framebuffer();
        IFramebuffer* up_fbo = ctx_->framebuffer(bloom_up_fbo_[i]);
        if (!bloom_up_fbo_[i].is_valid() || !up_fbo || !up_fbo->create(w, h)) return false;
        up_fbo->attach_color_texture(up_tex);
        if (!up_fbo->is_complete()) return false;

        w = std::max(16, w / 2);
        h = std::max(16, h / 2);
    }
    bloom_targets_valid_ = true;
    return true;
}

void RenderForwardClustered::_destroy_bloom_targets() {
    if (!ctx_) return;
    for (auto& fbo : bloom_down_fbo_) {
        if (fbo.is_valid()) { ctx_->destroy_framebuffer(fbo); fbo = {}; }
    }
    for (auto& fbo : bloom_up_fbo_) {
        if (fbo.is_valid()) { ctx_->destroy_framebuffer(fbo); fbo = {}; }
    }
    for (auto& tex : bloom_down_tex_) {
        if (tex.is_valid()) { ctx_->destroy_texture(tex); tex = {}; }
    }
    for (auto& tex : bloom_up_tex_) {
        if (tex.is_valid()) { ctx_->destroy_texture(tex); tex = {}; }
    }
    bloom_targets_valid_ = false;
}

bool RenderForwardClustered::_create_ssao_targets() {
    ssao_w_ = std::max(16, viewport_width_ / 2);
    ssao_h_ = std::max(16, viewport_height_ / 2);

    // 1x1 白回退纹理
    if (!ssao_fallback_tex_.is_valid()) {
        ssao_fallback_tex_ = ctx_->create_texture();
        ITexture* ft = ctx_->texture(ssao_fallback_tex_);
        if (!ssao_fallback_tex_.is_valid() || !ft) return false;
        const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        if (!ft->create(TextureFormat::RGBA16F, 1, 1, white)) return false;
        ft->set_filter(TextureFilter::Linear, TextureFilter::Linear);
        ft->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
    }

    for (int i = 0; i < 2; ++i) {
        ssao_tex_[i] = ctx_->create_texture();
        ITexture* tex = ctx_->texture(ssao_tex_[i]);
        if (!ssao_tex_[i].is_valid() || !tex ||
            !tex->create(TextureFormat::RGBA16F, ssao_w_, ssao_h_, nullptr)) {
            return false;
        }
        tex->set_filter(TextureFilter::Linear, TextureFilter::Linear);
        tex->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

        ssao_fbo_[i] = ctx_->create_framebuffer();
        IFramebuffer* fbo = ctx_->framebuffer(ssao_fbo_[i]);
        if (!ssao_fbo_[i].is_valid() || !fbo || !fbo->create(ssao_w_, ssao_h_)) return false;
        fbo->attach_color_texture(tex);
        if (!fbo->is_complete()) return false;
    }
    ssao_targets_valid_ = true;
    return true;
}

void RenderForwardClustered::_destroy_ssao_targets() {
    if (!ctx_) return;
    if (ssao_fallback_tex_.is_valid()) {
        ctx_->destroy_texture(ssao_fallback_tex_);
        ssao_fallback_tex_ = {};
    }
    for (auto& fbo : ssao_fbo_) {
        if (fbo.is_valid()) { ctx_->destroy_framebuffer(fbo); fbo = {}; }
    }
    for (auto& tex : ssao_tex_) {
        if (tex.is_valid()) { ctx_->destroy_texture(tex); tex = {}; }
    }
    ssao_targets_valid_ = false;
}

bool RenderForwardClustered::_create_taa_targets() {
    for (int i = 0; i < 2; ++i) {
        taa_tex_[i] = ctx_->create_texture();
        ITexture* tex = ctx_->texture(taa_tex_[i]);
        if (!taa_tex_[i].is_valid() || !tex ||
            !tex->create(TextureFormat::RGBA16F, viewport_width_, viewport_height_, nullptr)) {
            return false;
        }
        tex->set_filter(TextureFilter::Linear, TextureFilter::Linear);
        tex->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

        taa_fbo_[i] = ctx_->create_framebuffer();
        IFramebuffer* fbo = ctx_->framebuffer(taa_fbo_[i]);
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

void RenderForwardClustered::_destroy_taa_targets() {
    if (!ctx_) return;
    for (auto& fbo : taa_fbo_) {
        if (fbo.is_valid()) { ctx_->destroy_framebuffer(fbo); fbo = {}; }
    }
    for (auto& tex : taa_tex_) {
        if (tex.is_valid()) { ctx_->destroy_texture(tex); tex = {}; }
    }
    taa_targets_valid_ = false;
}

bool RenderForwardClustered::_create_auto_exposure_targets() {
    // 亮度链：L0 = w/2 ... L4 = w/32，L5 = 1x1
    int w = std::max(16, viewport_width_ / 2);
    int h = std::max(16, viewport_height_ / 2);
    for (int i = 0; i < k_lum_levels; ++i) {
        const int tw = (i == k_lum_levels - 1) ? 1 : w;
        const int th = (i == k_lum_levels - 1) ? 1 : h;
        lum_w_[i] = tw;
        lum_h_[i] = th;

        lum_tex_[i] = ctx_->create_texture();
        ITexture* tex = ctx_->texture(lum_tex_[i]);
        if (!lum_tex_[i].is_valid() || !tex ||
            !tex->create(TextureFormat::RGBA16F, tw, th, nullptr)) {
            GLOG_ERROR("RenderForwardClustered: auto-exposure lum texture {} failed", i);
            return false;
        }
        tex->set_filter(TextureFilter::Linear, TextureFilter::Linear);
        tex->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

        lum_fbo_[i] = ctx_->create_framebuffer();
        IFramebuffer* fbo = ctx_->framebuffer(lum_fbo_[i]);
        if (!lum_fbo_[i].is_valid() || !fbo || !fbo->create(tw, th)) return false;
        fbo->attach_color_texture(tex);
        if (!fbo->is_complete()) return false;

        w = std::max(2, w / 2);
        h = std::max(2, h / 2);
    }

    // 双缓冲曝光值（1x1 RGBA16F），初始化为 1.0 避免首帧全黑
    const float one[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    for (int i = 0; i < 2; ++i) {
        exposure_tex_[i] = ctx_->create_texture();
        ITexture* tex = ctx_->texture(exposure_tex_[i]);
        if (!exposure_tex_[i].is_valid() || !tex ||
            !tex->create(TextureFormat::RGBA16F, 1, 1, one)) {
            return false;
        }
        tex->set_filter(TextureFilter::Linear, TextureFilter::Linear);
        tex->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

        exposure_fbo_[i] = ctx_->create_framebuffer();
        IFramebuffer* fbo = ctx_->framebuffer(exposure_fbo_[i]);
        if (!exposure_fbo_[i].is_valid() || !fbo || !fbo->create(1, 1)) return false;
        fbo->attach_color_texture(tex);
        if (!fbo->is_complete()) return false;
    }
    auto_exposure_targets_valid_ = true;
    return true;
}

void RenderForwardClustered::_destroy_auto_exposure_targets() {
    if (!ctx_) return;
    for (auto& fbo : lum_fbo_) {
        if (fbo.is_valid()) { ctx_->destroy_framebuffer(fbo); fbo = {}; }
    }
    for (auto& tex : lum_tex_) {
        if (tex.is_valid()) { ctx_->destroy_texture(tex); tex = {}; }
    }
    for (auto& fbo : exposure_fbo_) {
        if (fbo.is_valid()) { ctx_->destroy_framebuffer(fbo); fbo = {}; }
    }
    for (auto& tex : exposure_tex_) {
        if (tex.is_valid()) { ctx_->destroy_texture(tex); tex = {}; }
    }
    auto_exposure_targets_valid_ = false;
}

// ---------------------------------------------------------------------------
// 后处理渲染
// ---------------------------------------------------------------------------

void RenderForwardClustered::_render_ssao(RenderContext* ctx) {
    if (pp_params_.ssao_enabled == 0 || !ssao_targets_valid_) return;
    if (!gtao_shader_.is_valid() || !ssao_blur_shader_.is_valid() || !fullscreen_mesh_.is_valid()) {
        return;
    }

    // 更新相机参数
    pp_params_.ssao_near = current_camera_.near_plane();
    pp_params_.ssao_far = current_camera_.far_plane();
    pp_params_.ssao_tan_half = std::tan(math::to_radians(current_camera_.fov()) * 0.5f);
    pp_params_.ssao_aspect = current_camera_.aspect();

    // 同步后处理参数到 shader
    for (RHIShaderHandle h : {gtao_shader_, ssao_blur_shader_}) {
        IShader* s = ctx->shader(h);
        if (s) s->set_post_process_params(pp_params_);
    }

    ctx->set_depth_test(false);
    ctx->set_cull_face(CullMode::None);
    ctx->set_blend(false);

    // Pass 1：GTAO（从深度重建视图位置，地平线搜索）
    ctx->set_framebuffer(ssao_fbo_[0]);
    ctx->set_viewport(0, 0, ssao_w_, ssao_h_);
    ctx->set_texture_raw_depth(gtao_shader_, fb_.depth_tex, TextureSlots::kTonemapHDR, "uDepthTexture");
    ctx->set_uniform_int(gtao_shader_, "uDepthTexture", TextureSlots::kTonemapHDR);
    ctx->draw_mesh(fullscreen_mesh_, gtao_shader_);

    // Pass 2：深度感知双边上模糊
    ctx->set_framebuffer(ssao_fbo_[1]);
    ctx->set_viewport(0, 0, ssao_w_, ssao_h_);
    ctx->set_texture(ssao_blur_shader_, ssao_tex_[0], TextureSlots::kTonemapHDR, "uTexture");
    ctx->set_uniform_int(ssao_blur_shader_, "uTexture", TextureSlots::kTonemapHDR);
    ctx->set_texture_raw_depth(ssao_blur_shader_, fb_.depth_tex, TextureSlots::kTAAHistory, "uDepthTexture");
    ctx->set_uniform_int(ssao_blur_shader_, "uDepthTexture", TextureSlots::kTAAHistory);
    ctx->draw_mesh(fullscreen_mesh_, ssao_blur_shader_);
}

void RenderForwardClustered::_render_bloom(RenderContext* ctx) {
    if (!bloom_targets_valid_ || !fullscreen_mesh_.is_valid()) return;
    if (!bloom_threshold_shader_.is_valid() || !bloom_downsample_shader_.is_valid() ||
        !bloom_upsample_shader_.is_valid()) {
        return;
    }

    ctx->set_depth_test(false);
    ctx->set_cull_face(CullMode::None);
    ctx->set_blend(false);

    // 同步后处理参数
    for (RHIShaderHandle h : {bloom_threshold_shader_, bloom_downsample_shader_, bloom_upsample_shader_}) {
        IShader* s = ctx->shader(h);
        if (s) s->set_post_process_params(pp_params_);
    }

    // 1. Threshold：HDR 全分辨率 → D0（半分辨率）
    {
        ctx->set_framebuffer(bloom_down_fbo_[0]);
        ctx->set_viewport(0, 0, bloom_level_w_[0], bloom_level_h_[0]);
        ctx->set_texture(bloom_threshold_shader_, fb_.color_tex, TextureSlots::kTonemapHDR, "uTexture");
        ctx->set_uniform_int(bloom_threshold_shader_, "uTexture", TextureSlots::kTonemapHDR);
        ctx->set_uniform_float(bloom_threshold_shader_, "uBloomThreshold", pp_params_.bloom_threshold);
        ctx->draw_mesh(fullscreen_mesh_, bloom_threshold_shader_);
    }

    // 2. 降采样模糊链：D0→D1→D2→D3，D3→U4
    for (int i = 1; i < k_bloom_levels; ++i) {
        const bool last = (i == k_bloom_levels - 1);
        const RHIFramebufferHandle target = last ? bloom_up_fbo_[i] : bloom_down_fbo_[i];
        const RHITextureHandle src = bloom_down_tex_[i - 1];
        ctx->set_framebuffer(target);
        ctx->set_viewport(0, 0, bloom_level_w_[i], bloom_level_h_[i]);
        ctx->set_texture(bloom_downsample_shader_, src, TextureSlots::kTonemapHDR, "uTexture");
        ctx->set_uniform_int(bloom_downsample_shader_, "uTexture", TextureSlots::kTonemapHDR);
        ctx->draw_mesh(fullscreen_mesh_, bloom_downsample_shader_);
    }

    // 3. 上采样合成：U_i = blur(U_{i+1}) + D_i
    for (int i = k_bloom_levels - 2; i >= 0; --i) {
        ctx->set_framebuffer(bloom_up_fbo_[i]);
        ctx->set_viewport(0, 0, bloom_level_w_[i], bloom_level_h_[i]);
        ctx->set_texture(bloom_upsample_shader_, bloom_up_tex_[i + 1],
                         TextureSlots::kTonemapHDR, "uTextureA");
        ctx->set_uniform_int(bloom_upsample_shader_, "uTextureA", TextureSlots::kTonemapHDR);
        ctx->set_texture(bloom_upsample_shader_, bloom_down_tex_[i],
                         TextureSlots::kTonemapBloom, "uTextureB");
        ctx->set_uniform_int(bloom_upsample_shader_, "uTextureB", TextureSlots::kTonemapBloom);
        ctx->draw_mesh(fullscreen_mesh_, bloom_upsample_shader_);
    }
}

void RenderForwardClustered::_render_auto_exposure(RenderContext* ctx) {
    if (pp_params_.auto_exposure == 0 || !auto_exposure_targets_valid_) return;
    if (!lum_average_shader_.is_valid() || !exposure_update_shader_.is_valid() ||
        !fullscreen_mesh_.is_valid()) {
        return;
    }

    ctx->set_depth_test(false);
    ctx->set_cull_face(CullMode::None);
    ctx->set_blend(false);

    for (RHIShaderHandle h : {lum_average_shader_, exposure_update_shader_}) {
        IShader* s = ctx->shader(h);
        if (s) s->set_post_process_params(pp_params_);
    }

    // 亮度链：HDR → L0 ... L4 → 1x1
    RHITextureHandle src = fb_.color_tex;
    for (int i = 0; i < k_lum_levels; ++i) {
        ctx->set_framebuffer(lum_fbo_[i]);
        ctx->set_viewport(0, 0, lum_w_[i], lum_h_[i]);
        ctx->set_texture(lum_average_shader_, src, TextureSlots::kTonemapHDR, "uTexture");
        ctx->set_uniform_int(lum_average_shader_, "uTexture", TextureSlots::kTonemapHDR);
        ctx->draw_mesh(fullscreen_mesh_, lum_average_shader_);
        src = lum_tex_[i];
    }

    // 曝光更新
    const int write_idx = exposure_ping_;
    ctx->set_framebuffer(exposure_fbo_[write_idx]);
    ctx->set_viewport(0, 0, 1, 1);
    ctx->set_texture(exposure_update_shader_, lum_tex_[k_lum_levels - 1],
                     TextureSlots::kTonemapHDR, "uTexture");
    ctx->set_uniform_int(exposure_update_shader_, "uTexture", TextureSlots::kTonemapHDR);
    ctx->set_texture(exposure_update_shader_, exposure_tex_[1 - write_idx],
                     TextureSlots::kTAAHistory, "uPrevExposure");
    ctx->set_uniform_int(exposure_update_shader_, "uPrevExposure", TextureSlots::kTAAHistory);
    ctx->draw_mesh(fullscreen_mesh_, exposure_update_shader_);

    current_exposure_idx_ = write_idx;
    exposure_ping_ = 1 - write_idx;
}

void RenderForwardClustered::_render_taa(RenderContext* ctx) {
    if (pp_params_.taa_enabled == 0 || !taa_targets_valid_) return;
    if (!taa_resolve_shader_.is_valid() || !fullscreen_mesh_.is_valid()) return;

    const int read = 1 - taa_ping_;
    const int write = taa_ping_;

    ctx->set_depth_test(false);
    ctx->set_cull_face(CullMode::None);
    ctx->set_blend(false);

    if (IShader* s = ctx->shader(taa_resolve_shader_)) {
        s->set_post_process_params(pp_params_);
    }

    ctx->set_framebuffer(taa_fbo_[write]);
    ctx->set_viewport(0, 0, viewport_width_, viewport_height_);
    ctx->set_texture(taa_resolve_shader_, fb_.color_tex, TextureSlots::kTonemapHDR, "uHDRTexture");
    ctx->set_uniform_int(taa_resolve_shader_, "uHDRTexture", TextureSlots::kTonemapHDR);
    ctx->set_texture(taa_resolve_shader_, taa_tex_[read], TextureSlots::kTAAHistory, "uHistoryTexture");
    ctx->set_uniform_int(taa_resolve_shader_, "uHistoryTexture", TextureSlots::kTAAHistory);
    ctx->draw_mesh(fullscreen_mesh_, taa_resolve_shader_);

    taa_ping_ = write;
}

void RenderForwardClustered::_render_tonemap(RenderContext* ctx) {
    if (!tonemap_shader_.is_valid() || !fb_.color_tex.is_valid() || !fullscreen_mesh_.is_valid()) {
        return;
    }

    // TAA 开启时读取 TAA 解析后的 HDR，否则读原始 HDR
    const bool use_taa = pp_params_.taa_enabled != 0 && taa_targets_valid_;
    const RHITextureHandle hdr_in = use_taa ? taa_tex_[taa_ping_] : fb_.color_tex;

    // 编辑器视口输出开启时写入独立 FBO
    const bool to_viewport = viewport_output_enabled_ && fb_.viewport_fbo.is_valid();

    ctx->set_framebuffer(to_viewport ? fb_.viewport_fbo : RHIFramebufferHandle{});
    ctx->set_viewport(0, 0, viewport_width_, viewport_height_);
    ctx->set_depth_test(false);
    ctx->set_cull_face(CullMode::None);
    ctx->set_blend(false);

    if (IShader* s = ctx->shader(tonemap_shader_)) {
        s->set_post_process_params(pp_params_);
    }

    ctx->set_texture(tonemap_shader_, hdr_in, TextureSlots::kTonemapHDR, "uHDRTexture");
    ctx->set_uniform_int(tonemap_shader_, "uHDRTexture", TextureSlots::kTonemapHDR);

    // Bloom 输入
    const RHITextureHandle bloom_tex =
        (pp_params_.bloom_enabled != 0 && bloom_targets_valid_) ? bloom_up_tex_[0] : hdr_in;
    ctx->set_texture(tonemap_shader_, bloom_tex, TextureSlots::kTonemapBloom, "uBloomTexture");
    ctx->set_uniform_int(tonemap_shader_, "uBloomTexture", TextureSlots::kTonemapBloom);

    // 自动曝光值
    const RHITextureHandle exp_in =
        (exposure_tex_[current_exposure_idx_].is_valid() && auto_exposure_targets_valid_)
            ? exposure_tex_[current_exposure_idx_]
            : hdr_in;
    ctx->set_texture(tonemap_shader_, exp_in, TextureSlots::kTonemapExposure, "uExposureTexture");
    ctx->set_uniform_int(tonemap_shader_, "uExposureTexture", TextureSlots::kTonemapExposure);

    ctx->draw_mesh(fullscreen_mesh_, tonemap_shader_);
}

// ---------------------------------------------------------------------------
// SSS 目标创建/销毁
// ---------------------------------------------------------------------------

bool RenderForwardClustered::_create_sss_targets() {
    if (!subsurface_scattering_) return false;
    return subsurface_scattering_->create_targets(viewport_width_, viewport_height_);
}

void RenderForwardClustered::_destroy_sss_targets() {
    if (!subsurface_scattering_) return;
    subsurface_scattering_->destroy_targets();
}

} // namespace gryce_engine::render