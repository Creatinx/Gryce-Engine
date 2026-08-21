#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "render/export.h"
#include "render/rhi_handle.h"
#include "render/shader.h"                     // PostProcessParams
#include "render/renderer_scene_render.h"
#include "render/renderer_rd/cluster_builder_rd.h"
#include "render/renderer_rd/forward_clustered/scene_shader_forward_clustered.h"
#include "render/renderer_rd/forward_clustered/shadow_system_rd.h"
#include "render/renderer_rd/effects/ssr.h"
#include "render/renderer_rd/effects/ssil.h"
#include "render/renderer_rd/effects/bokeh_dof.h"
#include "render/renderer_rd/effects/fsr2.h"
#include "render/renderer_rd/effects/subsurface_scattering.h"
#include "render/renderer_rd/environment/fog.h"
#include "render/renderer_rd/environment/sdfgi.h"
#include "render/renderer_rd/environment/reflection_probe.h"
#include "render/storage_rd/decal_storage.h"
#include "math/camera.h"
#include "scene/entity.h"

namespace gryce_engine::render {

class RenderContext;
class ITexture;
class IFramebuffer;

// ---------------------------------------------------------------------------
// RenderForwardClustered — Forward Clustered 场景渲染器
// 类似于 Godot 的 RenderForwardClustered，实现桌面级 Forward Clustered 渲染管线。
//
// 渲染流程：
// 1. _setup_cluster — 构建 Cluster 光剔除
// 2. _render_shadows — 渲染阴影贴图
// 3. _render_depth_prepass — Depth Prepass（可选法线+粗糙度）
// 4. _render_motion_vectors — 运动向量 pass
// 5. _render_opaque_pass — 不透明物体 Forward PBR + Cluster 光照
// 6. _render_sky — 天空盒
// 7. _render_alpha_pass — 透明物体
// 8. _render_post_processing — 后处理链
// ---------------------------------------------------------------------------
class GRYCE_RENDERER_API RenderForwardClustered : public RendererSceneRender {
public:
    // 渲染列表类型（与 Godot 一致）
    enum RenderListType : uint8_t {
        RENDER_LIST_OPAQUE = 0,       // 不透明物体
        RENDER_LIST_MOTION,           // 运动向量
        RENDER_LIST_ALPHA,            // 透明物体
        RENDER_LIST_SECONDARY,        // 阴影等
        RENDER_LIST_MAX
    };

    // Pass 模式
    enum PassMode : uint8_t {
        PASS_MODE_COLOR = 0,
        PASS_MODE_SHADOW,
        PASS_MODE_DEPTH,
        PASS_MODE_DEPTH_NORMAL_ROUGHNESS,
        PASS_MODE_DEPTH_MATERIAL,
        PASS_MODE_MOTION_VECTORS,
        PASS_MODE_MAX
    };

    // 渲染元素
    struct RenderElement {
        RHIMeshHandle mesh;
        uint32_t material_id = 0;
        math::Matrix4f transform;
        math::Matrix4f prev_transform;   // 上一帧 transform（运动向量）
        float distance_sq = 0.0f;        // 到相机的距离（透明排序）
        uint32_t sort_key = 0;           // 排序键
        uint32_t variant_key = 0;        // shader 变体键
        bool skinned = false;
    };

    RenderForwardClustered();
    ~RenderForwardClustered() override;

    // RendererSceneRender 接口
    bool init(RenderContext* ctx, const std::string& shader_dir = "res:/shaders") override;
    void shutdown() override;
    void render_scene(RenderData& data) override;

    void set_viewport_output_enabled(bool enabled) override;
    bool viewport_output_enabled() const override;
    ITexture* viewport_color_texture() const override;
    bool resize_render_targets(int width, int height) override;

    bool hot_reload() override;
    int poll_shader_hot_reload(RenderContext& ctx) override;

    // 渲染列表管理
    void clear_render_lists();
    void add_to_render_list(RenderListType list, const RenderElement& element);
    void sort_render_list(RenderListType list);
    const std::vector<RenderElement>& get_render_list(RenderListType list) const;

    // 访问子组件
    ClusterBuilderRD* cluster_builder() const { return cluster_builder_.get(); }
    SceneShaderForwardClustered* shader_system() const { return shader_system_.get(); }

    // 设置光源 SSBO 句柄（从 Compositor 传入）
    void set_light_buffer(RHIBufferHandle buffer) { light_buffer_ = buffer; }
    RHIBufferHandle light_buffer() const { return light_buffer_; }

    // 访问 Reflection Probe 系统
    ReflectionProbeRD* reflection_probes() const { return reflection_probes_.get(); }

private:
    // 渲染流程子步骤
    void _setup_cluster(const RenderData& data);
    void _render_shadows(RenderContext* ctx);
    void _render_depth_prepass(RenderContext* ctx);
    void _render_motion_vectors(RenderContext* ctx);
    void _render_opaque_pass(RenderContext* ctx);
    void _render_sky(RenderContext* ctx);
    void _render_alpha_pass(RenderContext* ctx);
    void _render_decals(RenderContext* ctx);
    void _render_post_processing(RenderContext* ctx);

    // 填充渲染列表（从场景收集 MeshRenderer/SkinnedMeshRenderer）
    void _populate_render_lists(const RenderData& data);

    // 计算 shader 变体键
    uint32_t _compute_variant_key(const scene::Entity* entity, bool transparent) const;

    // 设置光源 uniform
    void _set_light_uniforms(RenderContext* ctx, RHIShaderHandle shader);

    // 创建内部 framebuffer
    bool _create_internal_framebuffers(int width, int height);

    RenderContext* ctx_ = nullptr;
    bool initialized_ = false;

    // 子组件
    std::unique_ptr<ClusterBuilderRD> cluster_builder_;
    std::unique_ptr<SceneShaderForwardClustered> shader_system_;
    std::unique_ptr<ShadowSystemRD> shadow_system_;

    // 4 个渲染列表
    std::vector<RenderElement> render_lists_[RENDER_LIST_MAX];

    // 内部 framebuffer（含纹理+帧缓冲对）
    struct InternalFB {
        // 主颜色 RT
        RHITextureHandle color_tex;
        RHIFramebufferHandle color_fbo;
        // 深度 RT
        RHITextureHandle depth_tex;
        RHIFramebufferHandle depth_fbo;
        // 深度+法线+粗糙度 RT（Depth Prepass 输出）
        RHITextureHandle depth_normal_tex;
        RHIFramebufferHandle depth_normal_fbo;
        // 运动向量 RT
        RHITextureHandle motion_vectors_tex;
        RHIFramebufferHandle motion_vectors_fbo;
        // 视口输出 RT（编辑器离屏输出）
        RHITextureHandle viewport_tex;
        RHIFramebufferHandle viewport_fbo;
        int width = 0;
        int height = 0;
    } fb_;

    // 视口输出
    bool viewport_output_enabled_ = false;
    ITexture* viewport_color_tex_ = nullptr;

    // 光源 SSBO 句柄（从 Compositor 传入）
    RHIBufferHandle light_buffer_;

    // 光源数据（当前帧）
    std::vector<LightData> scene_lights_;

    // 渲染数据快照
    math::Camera current_camera_;
    scene::Scene* current_scene_ = nullptr;
    int viewport_width_ = 0;
    int viewport_height_ = 0;
    float delta_time_ = 0.0f;
    uint32_t frame_index_ = 0;
    math::Vector3f ambient_light_ = math::Vector3f(0.15f, 0.15f, 0.15f);

    // 后处理参数
    PostProcessParams pp_params_;

    // 后处理效果子系统
    std::unique_ptr<SSR_RD> ssr_;
    std::unique_ptr<SSIL_RD> ssil_;
    std::unique_ptr<BokehDOF_RD> bokeh_dof_;
    std::unique_ptr<FSR2_RD> fsr2_;
    std::unique_ptr<SubsurfaceScattering_RD> subsurface_scattering_;

    // 环境效果
    std::unique_ptr<VolumetricFog_RD> fog_;
    std::unique_ptr<SDFGI_RD> sdfgi_;
    std::unique_ptr<ReflectionProbeRD> reflection_probes_;

    // 贴花系统
    std::unique_ptr<DecalStorage> decal_storage_;
    RHIShaderHandle decal_shader_;
    RHIMeshHandle decal_box_mesh_;
    bool decal_initialized_ = false;

    // 全屏四边形（后处理用）
    RHIMeshHandle fullscreen_mesh_;

    // --- 后处理资源 ---
    // Bloom
    static constexpr int k_bloom_levels = 5;
    RHITextureHandle bloom_down_tex_[k_bloom_levels];
    RHIFramebufferHandle bloom_down_fbo_[k_bloom_levels];
    RHITextureHandle bloom_up_tex_[k_bloom_levels];
    RHIFramebufferHandle bloom_up_fbo_[k_bloom_levels];
    int bloom_level_w_[k_bloom_levels] = {};
    int bloom_level_h_[k_bloom_levels] = {};
    bool bloom_targets_valid_ = false;

    // SSAO
    int ssao_w_ = 0;
    int ssao_h_ = 0;
    RHITextureHandle ssao_tex_[2];
    RHIFramebufferHandle ssao_fbo_[2];
    RHITextureHandle ssao_fallback_tex_;
    bool ssao_targets_valid_ = false;

    // TAA
    RHITextureHandle taa_tex_[2];
    RHIFramebufferHandle taa_fbo_[2];
    int taa_ping_ = 0;
    bool taa_targets_valid_ = false;

    // 自动曝光
    static constexpr int k_lum_levels = 6;
    int lum_w_[k_lum_levels] = {};
    int lum_h_[k_lum_levels] = {};
    RHITextureHandle lum_tex_[k_lum_levels];
    RHIFramebufferHandle lum_fbo_[k_lum_levels];
    RHITextureHandle exposure_tex_[2];
    RHIFramebufferHandle exposure_fbo_[2];
    int current_exposure_idx_ = 0;
    int exposure_ping_ = 0;
    bool auto_exposure_targets_valid_ = false;

    // 后处理 shader 句柄
    RHIShaderHandle gtao_shader_;
    RHIShaderHandle ssao_blur_shader_;
    RHIShaderHandle bloom_threshold_shader_;
    RHIShaderHandle bloom_downsample_shader_;
    RHIShaderHandle bloom_upsample_shader_;
    RHIShaderHandle tonemap_shader_;
    RHIShaderHandle taa_resolve_shader_;
    RHIShaderHandle lum_average_shader_;
    RHIShaderHandle exposure_update_shader_;

    // 后处理创建/销毁
    bool _create_bloom_targets();
    void _destroy_bloom_targets();
    bool _create_ssao_targets();
    void _destroy_ssao_targets();
    bool _create_taa_targets();
    void _destroy_taa_targets();
    bool _create_auto_exposure_targets();
    void _destroy_auto_exposure_targets();
    bool _create_sss_targets();
    void _destroy_sss_targets();

    // 后处理渲染
    void _render_ssao(RenderContext* ctx);
    void _render_bloom(RenderContext* ctx);
    void _render_auto_exposure(RenderContext* ctx);
    void _render_taa(RenderContext* ctx);
    void _render_tonemap(RenderContext* ctx);

    // --- 天空盒资源 ---
    RHIShaderHandle skybox_shader_;
    RHITextureHandle skybox_tex_;
    bool skybox_initialized_ = false;
    math::Vector3f sky_sun_direction_ = math::Vector3f(0.3f, 0.8f, 0.5f);
    float sky_sun_intensity_ = 1.0f;
    math::Vector3f sky_color0_ = math::Vector3f(0.3f, 0.5f, 0.9f);   // 天顶
    math::Vector3f sky_color1_ = math::Vector3f(0.7f, 0.75f, 0.8f);  // 地平线
    math::Vector3f sky_color2_ = math::Vector3f(0.4f, 0.3f, 0.2f);   // 底部
    bool sky_use_texture_ = false;
};

} // namespace gryce_engine::render