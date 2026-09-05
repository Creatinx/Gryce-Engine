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

namespace {
// Decal 深度纹理采样器 slot（避免与现有 TextureSlots 冲突）
constexpr int kDecalDepthSlot = 41;
} // anonymous namespace

namespace {

// ---------------------------------------------------------------------------
// 网格本地包围球缓存：按 mesh_path 缓存本地中心与半径，避免每帧重复遍历顶点。
// ---------------------------------------------------------------------------
struct MeshBoundsCache {
    math::Vector3f center;
    float radius = 0.0f;
};

static std::unordered_map<std::string, MeshBoundsCache> g_mesh_bounds_cache;
static std::mutex g_mesh_bounds_mutex;

bool compute_world_mesh_bounds(const std::string& mesh_path, const math::Matrix4f& world,
                               math::Vector3f& out_center, float& out_radius) {
    MeshBoundsCache local;
    {
        std::lock_guard<std::mutex> lock(g_mesh_bounds_mutex);
        auto it = g_mesh_bounds_cache.find(mesh_path);
        if (it != g_mesh_bounds_cache.end()) {
            local = it->second;
        } else {
            auto mesh = assets::AssetManager::instance().load_mesh(mesh_path);
            if (!mesh || mesh->vertices.empty()) return false;

            math::Vector3f lo = mesh->vertices[0].position;
            math::Vector3f hi = lo;
            for (const auto& v : mesh->vertices) {
                lo = lo.min(v.position);
                hi = hi.max(v.position);
            }
            local.center = (lo + hi) * 0.5f;
            local.radius = (hi - local.center).length();
            g_mesh_bounds_cache[mesh_path] = local;
        }
    }

    // 世界空间中心
    out_center = world.transform_point(local.center);

    // 将本地半径按世界矩阵缩放：取各轴最大缩放作为保守半径。
    // 非均匀缩放会略有放大，但保证包围球一定包含物体。
    float scale_x = std::sqrt(world.m[0] * world.m[0] + world.m[1] * world.m[1] + world.m[2] * world.m[2]);
    float scale_y = std::sqrt(world.m[4] * world.m[4] + world.m[5] * world.m[5] + world.m[6] * world.m[6]);
    float scale_z = std::sqrt(world.m[8] * world.m[8] + world.m[9] * world.m[9] + world.m[10] * world.m[10]);
    out_radius = local.radius * std::max({scale_x, scale_y, scale_z});
    return true;
}

// ---------------------------------------------------------------------------
// 蒙皮模型本地包围球缓存：按 model_path 缓存所有 submesh 合并后的中心与半径。
// ---------------------------------------------------------------------------
struct SkinnedMeshBoundsCache {
    math::Vector3f center;
    float radius = 0.0f;
};

static std::unordered_map<std::string, SkinnedMeshBoundsCache> g_skinned_bounds_cache;
static std::mutex g_skinned_bounds_mutex;

bool compute_world_skinned_mesh_bounds(const std::string& model_path, const math::Matrix4f& world,
                                       math::Vector3f& out_center, float& out_radius) {
    SkinnedMeshBoundsCache local;
    {
        std::lock_guard<std::mutex> lock(g_skinned_bounds_mutex);
        auto it = g_skinned_bounds_cache.find(model_path);
        if (it != g_skinned_bounds_cache.end()) {
            local = it->second;
        } else {
            auto model = assets::AssetManager::instance().load_skinned_model(model_path);
            if (!model || model->meshes.empty()) return false;

            math::Vector3f lo = model->meshes[0].vertices[0].position;
            math::Vector3f hi = lo;
            for (const auto& mesh : model->meshes) {
                if (mesh.vertices.empty()) continue;
                for (const auto& v : mesh.vertices) {
                    lo = lo.min(v.position);
                    hi = hi.max(v.position);
                }
            }
            local.center = (lo + hi) * 0.5f;
            local.radius = (hi - local.center).length();
            g_skinned_bounds_cache[model_path] = local;
        }
    }

    out_center = world.transform_point(local.center);

    float scale_x = std::sqrt(world.m[0] * world.m[0] + world.m[1] * world.m[1] + world.m[2] * world.m[2]);
    float scale_y = std::sqrt(world.m[4] * world.m[4] + world.m[5] * world.m[5] + world.m[6] * world.m[6]);
    float scale_z = std::sqrt(world.m[8] * world.m[8] + world.m[9] * world.m[9] + world.m[10] * world.m[10]);
    out_radius = local.radius * std::max({scale_x, scale_y, scale_z});
    return true;
}

} // namespace

RenderPipeline::RenderPipeline() = default;

RenderPipeline::~RenderPipeline() {
    shutdown();
}

bool RenderPipeline::init(RenderContext* ctx, const std::string& shader_dir) {
    if (!ctx) return false;
    ctx_ = ctx;
    shader_dir_ = resources::ResourcePath::resolve(shader_dir);

    if (!create_cascade_shadow_maps(ctx)) {
        GLOG_ERROR("RenderPipeline: failed to create cascade shadow maps");
        return false;
    }

    // 初始化 Shadow Atlas（失败不阻塞，仅禁用 atlas 功能）
    if (!shadow_atlas_.init(ctx)) {
        GLOG_WARN("RenderPipeline: ShadowAtlas init failed, atlas disabled");
        shadow_atlas_enabled_ = false;
    }

    if (hdr_enabled_) {
        if (!create_hdr_target(ctx)) {
            GLOG_WARN("RenderPipeline: HDR target failed, falling back to LDR");
            hdr_enabled_ = false;
        }
    }

    // 编辑器视口离屏输出依赖 HDR 管线（tonemap 结果即 LDR 视口纹理）
    if (hdr_enabled_ && viewport_output_enabled_) {
        if (!create_viewport_target(ctx)) {
            GLOG_WARN("RenderPipeline: viewport output target failed, disabling");
            viewport_output_enabled_ = false;
        }
    }

    pbr_shader_ = load_shader("pbr", hdr_enabled_ ? hdr_fbo_ : RHIFramebufferHandle{}, true, false);
    if (!pbr_shader_.is_valid()) {
        GLOG_ERROR("RenderPipeline: failed to load pbr shader");
        return false;
    }

    // 蒙皮 PBR shader 为可选项：旧项目 shader 目录没有 skinned_pbr 时
    // 只告警降级（SkinnedMeshRenderer 不绘制），不影响普通渲染管线。
    skinned_pbr_shader_ = load_shader("skinned_pbr", hdr_enabled_ ? hdr_fbo_ : RHIFramebufferHandle{}, true, false, true);
    if (!skinned_pbr_shader_.is_valid()) {
        GLOG_WARN("RenderPipeline: skinned_pbr shader unavailable, skinned rendering disabled");
    }

    // Scene View 网格线 shader：可选，加载失败仅禁用网格线。
    grid_shader_ = load_shader("grid", hdr_enabled_ ? hdr_fbo_ : RHIFramebufferHandle{}, true, false);
    if (!grid_shader_.is_valid()) {
        GLOG_WARN("RenderPipeline: grid shader unavailable, viewport grid disabled");
    } else if (!create_grid_mesh(ctx)) {
        GLOG_WARN("RenderPipeline: grid mesh creation failed, viewport grid disabled");
        grid_shader_ = RHIShaderHandle{};
    }

    shadow_shader_ = load_shader("shadow_map", shadow_fbos_[0], false, false);
    if (!shadow_shader_.is_valid()) {
        GLOG_ERROR("RenderPipeline: failed to load shadow shader");
        return false;
    }

    // VSM/ESM 阴影 shader（可选：加载失败降级为 PCF）
    shadow_vsm_shader_ = load_shader("shadow_vsm", shadow_fbos_[0], false, false);
    if (!shadow_vsm_shader_.is_valid()) {
        GLOG_WARN("RenderPipeline: shadow_vsm shader unavailable, VSM disabled");
    }
    shadow_esm_shader_ = load_shader("shadow_esm", shadow_fbos_[0], false, false);
    if (!shadow_esm_shader_.is_valid()) {
        GLOG_WARN("RenderPipeline: shadow_esm shader unavailable, ESM disabled");
    }
    // VSM blur shader 需要全屏四边形的 FBO（后处理管线），用 shadow_fbos_[0] 作为占位
    // 实际使用时会在 render_vsm_blur 中设置正确的 FBO
    vsm_blur_shader_ = load_shader("vsm_blur", shadow_fbos_[0], false, false);
    if (!vsm_blur_shader_.is_valid()) {
        GLOG_WARN("RenderPipeline: vsm_blur shader unavailable, VSM blur disabled");
    }
    // 创建 VSM 彩色阴影目标
    if (!create_vsm_color_targets(ctx)) {
        GLOG_WARN("RenderPipeline: VSM color targets failed, VSM/ESM disabled");
    }

    // Shadow Atlas shader（使用 atlas FBO 作为渲染目标；失败不阻塞）
    if (shadow_atlas_.valid()) {
        shadow_atlas_shader_ = load_shader("shadow_atlas", shadow_atlas_.atlas_fbo(), false, false);
        if (!shadow_atlas_shader_.is_valid()) {
            GLOG_WARN("RenderPipeline: shadow_atlas shader unavailable, atlas disabled");
            shadow_atlas_enabled_ = false;
        }
    }

    // 点光源阴影（双抛物面映射）：需要 RGBA16F 颜色目标，用空 FBO 加载
    // 但在 FBO 创建之前无法加载 shader（需要 render pass），
    // 延迟到 create_point_shadow_targets 之后加载。
    // 先创建目标，再加载 shader（在点光源阴影目标创建后统一处理）

    // tonemap 管线的 render pass 必须与实际绘制目标一致：视口离屏输出开启时
    // 画到 viewport_fbo_（RGBA8、无深度），否则画到默认 framebuffer（交换链
    // 格式、含深度）。用错误的 render pass 建管线是 UB（格式/深度不匹配），
    // 在部分驱动上直接 VK_ERROR_DEVICE_LOST + 视口黑屏。
    const bool tonemap_to_viewport = viewport_output_enabled_ && viewport_fbo_.is_valid();
    tonemap_shader_ = load_shader("tonemap",
                                  tonemap_to_viewport ? viewport_fbo_ : RHIFramebufferHandle{},
                                  true, true);
    if (!tonemap_shader_.is_valid()) {
        GLOG_ERROR("RenderPipeline: failed to load tonemap shader");
        return false;
    }

    // 预先把所有级联 shadow sampler 绑定到固定 PBR shadow slot，避免首次使用
    // 时 sampler 指向 texture unit 0（默认纹理非 depth）触发 NVIDIA undefined
    // behavior warning。注意 set_uniform_int 前必须先 set_shader，否则 GL 报无 active program。
    auto bind_shadow_samplers = [this](RHIShaderHandle shader) {
        if (!shader.is_valid()) return;
        ctx_->set_shader(shader);
        static constexpr int slots[k_max_cascades] = {
            TextureSlots::kPBRShadow, TextureSlots::kPBRShadowC1,
            TextureSlots::kPBRShadowC2, TextureSlots::kPBRShadowC3,
        };
        for (int i = 0; i < k_max_cascades; ++i) {
            ITexture* shadow_map_ptr = ctx_->texture(shadow_maps_[i]);
            if (shadow_map_ptr) {
                shadow_map_ptr->bind(slots[i]);
            }
            ctx_->set_texture(shader, shadow_maps_[i], slots[i], "");
        }
        ctx_->set_uniform_int(shader, "uShadowMap", TextureSlots::kPBRShadow);
        ctx_->set_uniform_int(shader, "uShadowMap1", TextureSlots::kPBRShadowC1);
        ctx_->set_uniform_int(shader, "uShadowMap2", TextureSlots::kPBRShadowC2);
        ctx_->set_uniform_int(shader, "uShadowMap3", TextureSlots::kPBRShadowC3);
    };
    bind_shadow_samplers(pbr_shader_);
    bind_shadow_samplers(skinned_pbr_shader_);

    if (hdr_enabled_) {
        if (!create_fullscreen_mesh(ctx)) {
            GLOG_WARN("RenderPipeline: fullscreen mesh failed, falling back to LDR");
            hdr_enabled_ = false;
        }
    }

    // Bloom 后处理目标 + shader（仅 HDR 管线）
    if (hdr_enabled_ && !create_bloom_targets(ctx)) {
        GLOG_WARN("RenderPipeline: bloom targets failed, bloom disabled");
        pp_params_.bloom_enabled = 0;
    }
    if (hdr_enabled_ && bloom_targets_valid_) {
        bloom_threshold_shader_ = load_shader("bloom_threshold", bloom_down_fbo_[0], true, true);
        bloom_downsample_shader_ = load_shader("bloom_downsample", bloom_down_fbo_[1], true, true);
        bloom_upsample_shader_ = load_shader("bloom_upsample", bloom_up_fbo_[0], true, true);
        if (!bloom_threshold_shader_.is_valid() || !bloom_downsample_shader_.is_valid() ||
            !bloom_upsample_shader_.is_valid()) {
            GLOG_WARN("RenderPipeline: bloom shaders unavailable, bloom disabled");
            pp_params_.bloom_enabled = 0;
        }
    }

    // 自动曝光 + TAA 目标与 shader（仅 HDR 管线，可选）
    if (hdr_enabled_) {
        if (create_auto_exposure_targets(ctx)) {
            lum_average_shader_ = load_shader("lum_average", lum_fbo_[0], true, true);
            exposure_update_shader_ = load_shader("exposure_update", exposure_fbo_[0], true, true);
            if (!lum_average_shader_.is_valid() || !exposure_update_shader_.is_valid()) {
                GLOG_WARN("RenderPipeline: auto-exposure shaders unavailable, disabled");
                pp_params_.auto_exposure = 0;
            }
        } else {
            GLOG_WARN("RenderPipeline: auto-exposure targets failed, disabled");
            pp_params_.auto_exposure = 0;
        }

        if (create_taa_targets(ctx)) {
            taa_resolve_shader_ = load_shader("taa_resolve", taa_fbo_[0], true, true);
            if (!taa_resolve_shader_.is_valid()) {
                GLOG_WARN("RenderPipeline: TAA shader unavailable, disabled");
                pp_params_.taa_enabled = 0;
            }
        } else {
            GLOG_WARN("RenderPipeline: TAA targets failed, disabled");
            pp_params_.taa_enabled = 0;
        }

        if (create_ssao_targets(ctx)) {
            gtao_shader_ = load_shader("gtao", ssao_fbo_[0], true, true);
            ssao_blur_shader_ = load_shader("ssao_blur", ssao_fbo_[1], true, true);
            if (!gtao_shader_.is_valid() || !ssao_blur_shader_.is_valid()) {
                GLOG_WARN("RenderPipeline: GTAO shaders unavailable, disabled");
                pp_params_.ssao_enabled = 0;
            }
        } else {
            GLOG_WARN("RenderPipeline: GTAO targets failed, disabled");
            pp_params_.ssao_enabled = 0;
        }

        if (create_contact_shadow_targets(ctx)) {
            contact_shadow_shader_ = load_shader("contact_shadow", contact_shadow_fbo_, true, true);
            if (!contact_shadow_shader_.is_valid()) {
                GLOG_WARN("RenderPipeline: contact shadow shader unavailable, disabled");
                contact_shadow_enabled_ = false;
            }
        } else {
            GLOG_WARN("RenderPipeline: contact shadow targets failed, disabled");
            contact_shadow_enabled_ = false;
        }
    }

    // ---- Reflection Probe 初始化 ----
    if (!probe_system_.init(ctx)) {
        GLOG_WARN("RenderPipeline: ReflectionProbeRD init failed, probe system disabled");
        probe_system_enabled_ = false;
    }

    // ---- Decal 初始化 ----
    decal_storage_.init(ctx);
    // 加载 decal shader（后处理管线，从 HDR 深度读取，blend 到 HDR 颜色）
    decal_shader_ = load_shader("decal", hdr_enabled_ ? hdr_fbo_ : RHIFramebufferHandle{}, true, false);
    if (!decal_shader_.is_valid()) {
        GLOG_WARN("RenderPipeline: decal shader unavailable, decal rendering disabled");
        decal_enabled_ = false;
    }
    // 创建 decal 包围盒 mesh（单位立方体）
    if (decal_shader_.is_valid() && !create_decal_box_mesh(ctx)) {
        GLOG_WARN("RenderPipeline: decal box mesh creation failed, decal rendering disabled");
        decal_enabled_ = false;
    }

    // ---- Deferred Rendering（可选，需要 HDR 管线） ----
    if (deferred_enabled_ && hdr_enabled_) {
        if (!create_gbuffer_targets(ctx)) {
            GLOG_WARN("RenderPipeline: gbuffer targets failed, fallback to forward");
            deferred_enabled_ = false;
        }
        if (deferred_enabled_) {
            gbuffer_shader_ = load_shader("g_buffer", gbuffer_fbo_, true, false);
            if (!gbuffer_shader_.is_valid()) {
                GLOG_WARN("RenderPipeline: g_buffer shader unavailable, fallback to forward");
                deferred_enabled_ = false;
            }
        }
        if (deferred_enabled_) {
            deferred_lighting_shader_ = load_shader("deferred_lighting", hdr_fbo_, true, false);
            if (!deferred_lighting_shader_.is_valid()) {
                GLOG_WARN("RenderPipeline: deferred_lighting shader unavailable, fallback to forward");
                deferred_enabled_ = false;
            }
        }
        if (!deferred_enabled_) {
            destroy_gbuffer_targets();
        }
    }

    // ---- 点光源双抛物面阴影（可选） ----
    if (point_shadow_enabled_ && !create_point_shadow_targets(ctx)) {
        GLOG_WARN("RenderPipeline: point shadow targets failed, point shadow disabled");
        point_shadow_enabled_ = false;
    }
    if (point_shadow_enabled_ && !point_shadow_fbo_.empty() && point_shadow_fbo_[0].is_valid()) {
        point_shadow_shader_ = load_shader("point_shadow_map", point_shadow_fbo_[0], true, false);
        if (!point_shadow_shader_.is_valid()) {
            GLOG_WARN("RenderPipeline: point_shadow_map shader unavailable, point shadow disabled");
            point_shadow_enabled_ = false;
        }
    } else {
        point_shadow_enabled_ = false;
    }
    if (!point_shadow_enabled_) {
        destroy_point_shadow_targets();
    }

    // SSR 屏幕空间反射
    ssr_.init(ctx);

    // 体积雾
    fog_.init(ctx, shader_dir_);

    // Bokeh DOF 景深
    dof_.init(ctx);
    if (hdr_enabled_ && dof_.valid()) {
        dof_.create_targets(viewport_width_, viewport_height_);
    }

    // Motion Blur 运动模糊
    motion_blur_.init(ctx, shader_dir_);
    if (hdr_enabled_ && motion_blur_.valid()) {
        motion_blur_.create_targets(viewport_width_, viewport_height_);
    }

    // ---- GI 全局光照系统初始化 ----
    sdfgi_.init(ctx);
    voxel_gi_.init(ctx);
    ssil_.init(ctx);
    if (hdr_enabled_ && ssil_.valid()) {
        ssil_.create_targets(viewport_width_, viewport_height_);
    }

    // ---- 水面渲染初始化 ----
    water_.init(ctx, shader_dir_);
    if (hdr_enabled_ && water_.valid()) {
        water_.create_targets(viewport_width_, viewport_height_);
    }

    initialized_ = true;
    GLOG_INFO("RenderPipeline initialized (PBR + multi-light + CSM + {}{}{}{}{}{}{}{})",
              hdr_enabled_ ? "HDR" : "LDR", pp_params_.bloom_enabled ? " + bloom" : "",
              deferred_enabled_ ? " + deferred" : "",
              ssr_.valid() ? " + SSR" : "",
              fog_.valid() ? " + Fog" : "",
              sdfgi_.valid() ? " + SDFGI" : "",
              voxel_gi_.valid() ? " + VoxelGI" : "",
              ssil_.valid() ? " + SSIL" : "");
    return true;
}

void RenderPipeline::shutdown() {
    if (!ctx_) return;

    // 重建后材质必须重新 bind（否则 hot_reload 后首帧会因缓存跳过而用默认 uniform）
    last_bound_material_pbr_ = nullptr;
    last_bound_material_skinned_ = nullptr;

    clear_skybox();
    clear_environment();

    if (fullscreen_mesh_.is_valid()) {
        ctx_->destroy_mesh(fullscreen_mesh_);
        fullscreen_mesh_ = RHIMeshHandle{};
    }
    if (hdr_fbo_.is_valid()) {
        ctx_->destroy_framebuffer(hdr_fbo_);
        hdr_fbo_ = RHIFramebufferHandle{};
    }
    if (hdr_color_.is_valid()) {
        ctx_->destroy_texture(hdr_color_);
        hdr_color_ = RHITextureHandle{};
    }
    if (hdr_depth_.is_valid()) {
        ctx_->destroy_texture(hdr_depth_);
        hdr_depth_ = RHITextureHandle{};
    }
    if (viewport_fbo_.is_valid()) {
        ctx_->destroy_framebuffer(viewport_fbo_);
        viewport_fbo_ = RHIFramebufferHandle{};
    }
    if (viewport_color_.is_valid()) {
        ctx_->destroy_texture(viewport_color_);
        viewport_color_ = RHITextureHandle{};
    }
    if (owns_shaders_ && tonemap_shader_.is_valid()) {
        ctx_->destroy_shader(tonemap_shader_);
        tonemap_shader_ = RHIShaderHandle{};
    }
    if (owns_shaders_ && bloom_threshold_shader_.is_valid()) {
        ctx_->destroy_shader(bloom_threshold_shader_);
        bloom_threshold_shader_ = RHIShaderHandle{};
    }
    if (owns_shaders_ && bloom_downsample_shader_.is_valid()) {
        ctx_->destroy_shader(bloom_downsample_shader_);
        bloom_downsample_shader_ = RHIShaderHandle{};
    }
    if (owns_shaders_ && bloom_upsample_shader_.is_valid()) {
        ctx_->destroy_shader(bloom_upsample_shader_);
        bloom_upsample_shader_ = RHIShaderHandle{};
    }
    if (owns_shaders_ && lum_average_shader_.is_valid()) {
        ctx_->destroy_shader(lum_average_shader_);
        lum_average_shader_ = RHIShaderHandle{};
    }
    if (owns_shaders_ && exposure_update_shader_.is_valid()) {
        ctx_->destroy_shader(exposure_update_shader_);
        exposure_update_shader_ = RHIShaderHandle{};
    }
    if (owns_shaders_ && taa_resolve_shader_.is_valid()) {
        ctx_->destroy_shader(taa_resolve_shader_);
        taa_resolve_shader_ = RHIShaderHandle{};
    }
    if (owns_shaders_ && gtao_shader_.is_valid()) {
        ctx_->destroy_shader(gtao_shader_);
        gtao_shader_ = RHIShaderHandle{};
    }
    if (owns_shaders_ && ssao_blur_shader_.is_valid()) {
        ctx_->destroy_shader(ssao_blur_shader_);
        ssao_blur_shader_ = RHIShaderHandle{};
    }
    if (owns_shaders_ && contact_shadow_shader_.is_valid()) {
        ctx_->destroy_shader(contact_shadow_shader_);
        contact_shadow_shader_ = RHIShaderHandle{};
    }
    if (owns_shaders_ && point_shadow_shader_.is_valid()) {
        ctx_->destroy_shader(point_shadow_shader_);
        point_shadow_shader_ = RHIShaderHandle{};
    }
    if (owns_shaders_ && shadow_vsm_shader_.is_valid()) {
        ctx_->destroy_shader(shadow_vsm_shader_);
        shadow_vsm_shader_ = RHIShaderHandle{};
    }
    if (owns_shaders_ && shadow_esm_shader_.is_valid()) {
        ctx_->destroy_shader(shadow_esm_shader_);
        shadow_esm_shader_ = RHIShaderHandle{};
    }
    if (owns_shaders_ && vsm_blur_shader_.is_valid()) {
        ctx_->destroy_shader(vsm_blur_shader_);
        vsm_blur_shader_ = RHIShaderHandle{};
    }
    destroy_vsm_color_targets();
    destroy_point_shadow_targets();
    destroy_contact_shadow_targets();
    destroy_bloom_targets();
    destroy_auto_exposure_targets();
    destroy_taa_targets();
    destroy_ssao_targets();
    destroy_gbuffer_targets();
    if (owns_shaders_ && gbuffer_shader_.is_valid()) {
        ctx_->destroy_shader(gbuffer_shader_);
        gbuffer_shader_ = RHIShaderHandle{};
    }
    if (owns_shaders_ && deferred_lighting_shader_.is_valid()) {
        ctx_->destroy_shader(deferred_lighting_shader_);
        deferred_lighting_shader_ = RHIShaderHandle{};
    }
    if (lut_texture_.is_valid()) {
        ctx_->destroy_texture(lut_texture_);
        lut_texture_ = RHITextureHandle{};
    }

    if (grid_mesh_.is_valid()) {
        ctx_->destroy_mesh(grid_mesh_);
        grid_mesh_ = RHIMeshHandle{};
    }
    if (owns_shaders_ && grid_shader_.is_valid()) {
        ctx_->destroy_shader(grid_shader_);
        grid_shader_ = RHIShaderHandle{};
    }

    for (auto& fb : shadow_fbos_) {
        if (fb.is_valid()) {
            ctx_->destroy_framebuffer(fb);
            fb = RHIFramebufferHandle{};
        }
    }
    for (auto& tex : shadow_maps_) {
        if (tex.is_valid()) {
            ctx_->destroy_texture(tex);
            tex = RHITextureHandle{};
        }
    }
    if (owns_shaders_) {
        if (pbr_shader_.is_valid()) ctx_->destroy_shader(pbr_shader_);
        if (shadow_shader_.is_valid()) ctx_->destroy_shader(shadow_shader_);
        if (shadow_atlas_shader_.is_valid()) ctx_->destroy_shader(shadow_atlas_shader_);
        if (skinned_pbr_shader_.is_valid()) ctx_->destroy_shader(skinned_pbr_shader_);
    }
    pbr_shader_ = RHIShaderHandle{};
    shadow_shader_ = RHIShaderHandle{};
    shadow_atlas_shader_ = RHIShaderHandle{};
    skinned_pbr_shader_ = RHIShaderHandle{};

    shadow_atlas_.destroy();

    ssr_.destroy();
    fog_.destroy();
    dof_.destroy();
    motion_blur_.destroy();
    sdfgi_.destroy();
    voxel_gi_.destroy();
    ssil_.destroy();
    water_.destroy();

    // Reflection Probe / Decal 清理
    probe_system_.destroy();
    decal_storage_.destroy();
    if (owns_shaders_ && decal_shader_.is_valid()) {
        ctx_->destroy_shader(decal_shader_);
        decal_shader_ = RHIShaderHandle{};
    }
    if (decal_box_mesh_.is_valid()) {
        ctx_->destroy_mesh(decal_box_mesh_);
        decal_box_mesh_ = RHIMeshHandle{};
    }

    ctx_ = nullptr;
    initialized_ = false;
}

RHIShaderHandle RenderPipeline::load_shader(const std::string& name, RHIFramebufferHandle target, bool color_output, bool post_process,
                                            bool skinned) {
    RHIShaderHandle shader = ctx_->create_shader();
    IShader* shader_ptr = ctx_->shader(shader);
    IFramebuffer* target_ptr = ctx_->framebuffer(target);
    if (!shader.is_valid() || !shader_ptr || !shader_ptr->load_program(name, shader_dir_, target_ptr, color_output, post_process, false, skinned)) {
        GLOG_ERROR("RenderPipeline: failed to load shader program '{}'", name);
        if (shader.is_valid()) {
            ctx_->destroy_shader(shader);
        }
        return RHIShaderHandle{};
    }
    owns_shaders_ = true;
    return shader;
}

int RenderPipeline::poll_shader_hot_reload(RenderContext& ctx) {
    if (!initialized_ || !ctx_) return 0;

    std::vector<RHIShaderHandle> to_reload;
    auto check = [&](RHIShaderHandle h) {
        if (!h.is_valid()) return;
        IShader* s = ctx_->shader(h);
        if (s && s->shader_files_changed()) {
            to_reload.push_back(h);
        }
    };
    check(pbr_shader_);
    check(shadow_shader_);
    check(skinned_pbr_shader_);
    check(grid_shader_);
    check(skybox_shader_);
    check(tonemap_shader_);
    check(bloom_threshold_shader_);
    check(bloom_downsample_shader_);
    check(bloom_upsample_shader_);
    check(lum_average_shader_);
    check(exposure_update_shader_);
    check(taa_resolve_shader_);
    check(gtao_shader_);
    check(ssao_blur_shader_);
    check(decal_shader_);
    check(point_shadow_shader_);
    check(shadow_vsm_shader_);
    check(shadow_esm_shader_);
    check(vsm_blur_shader_);

    if (to_reload.empty()) return 0;

    // 线程约束：pause 后 GL context 回到主线程，shader 的 GL/VK 调用在此执行
    ctx.pause_render_thread();
    int reloaded = 0;
    for (RHIShaderHandle h : to_reload) {
        IShader* s = ctx_->shader(h);
        if (s && s->reload()) {
            ++reloaded;
        }
    }
    ctx.resume_render_thread();

    if (reloaded > 0) {
        GLOG_INFO("RenderPipeline: hot-reloaded {} shader(s)", reloaded);
    }
    return reloaded;
}

void RenderPipeline::set_camera(const math::Camera& camera) {
    camera_ = const_cast<math::Camera*>(&camera);

    // 3D 空间音频的监听点应跟随主摄像机；此函数每帧被调用。
    // 未初始化时 set_listener_position 是安全的 no-op。
    audio::AudioEngine::instance().set_listener_position(camera.position());
}

void RenderPipeline::set_lights(const std::vector<Light>& lights) {
    // 多光源管理：最多 k_max_lights 个。第一个方向光（阴影投射）保留，
    // 其余按 亮度 / (1 + 到相机距离^2) 优先级排序，超出部分截断。
    lights_.clear();
    lights_.reserve(std::min<size_t>(lights.size(), k_max_lights));
    std::vector<Light> rest;
    rest.reserve(lights.size());
    bool has_shadow_light = false;
    for (const auto& l : lights) {
        if (!has_shadow_light && l.type == LightType::Directional) {
            lights_.push_back(l);
            has_shadow_light = true;
        } else {
            rest.push_back(l);
        }
    }
    const math::Vector3f cam_pos = camera_ ? camera_->position() : math::Vector3f::zero();
    auto priority = [&](const Light& l) {
        if (l.type == LightType::Directional) return l.intensity;
        return l.intensity / (1.0f + (l.position - cam_pos).length_sq());
    };
    std::sort(rest.begin(), rest.end(),
              [&](const Light& a, const Light& b) { return priority(a) > priority(b); });
    for (const auto& l : rest) {
        if (lights_.size() >= static_cast<size_t>(k_max_lights)) break;
        lights_.push_back(l);
    }
}

void RenderPipeline::set_viewport(int width, int height) {
    viewport_width_ = width;
    viewport_height_ = height;
}

void RenderPipeline::set_ssr_params(float max_steps, float fade_range) {
    ssr_max_steps_ = max_steps;
    ssr_fade_range_ = fade_range;
}

void RenderPipeline::set_fog_params(const math::Vector3f& color, float density, float height) {
    fog_color_ = color;
    fog_density_ = density;
    fog_height_ = height;
}

bool RenderPipeline::Frustum::contains_sphere(const math::Vector3f& center, float radius) const {
    for (int i = 0; i < 6; ++i) {
        const math::Vector3f normal(planes[i].x, planes[i].y, planes[i].z);
        float distance = normal.dot(center) + planes[i].w;
        if (distance < -radius) {
            return false;
        }
    }
    return true;
}

RenderPipeline::Frustum RenderPipeline::extract_frustum(const math::Matrix4f& vp) const {
    Frustum frustum;
    // 提取第 i 行（列主序：row i = m[i], m[i+4], m[i+8], m[i+12]）
    auto row = [&](int i) {
        return math::Vector4f(vp.m[i], vp.m[i + 4], vp.m[i + 8], vp.m[i + 12]);
    };

    frustum.planes[0] = row(3) + row(0); // Left
    frustum.planes[1] = row(3) - row(0); // Right
    frustum.planes[2] = row(3) + row(1); // Bottom
    frustum.planes[3] = row(3) - row(1); // Top
    frustum.planes[4] = row(3) + row(2); // Near
    frustum.planes[5] = row(3) - row(2); // Far

    for (int i = 0; i < 6; ++i) {
        const math::Vector3f normal(frustum.planes[i].x, frustum.planes[i].y, frustum.planes[i].z);
        float len = normal.length();
        if (len > 1e-6f) {
            frustum.planes[i] = frustum.planes[i] / len;
        }
    }
    return frustum;
}

bool RenderPipeline::is_inside_frustum(const Frustum& frustum, const math::Matrix4f& world_transform,
                                       const std::string& mesh_path) const {
    math::Vector3f center;
    float radius = 0.0f;
    if (!compute_world_mesh_bounds(mesh_path, world_transform, center, radius)) {
        // 无法计算边界时保守保留
        return true;
    }
    bool inside = frustum.contains_sphere(center, radius);
    return inside;
}

bool RenderPipeline::is_inside_frustum_skinned(const Frustum& frustum, const math::Matrix4f& world_transform,
                                               const std::string& model_path) const {
    math::Vector3f center;
    float radius = 0.0f;
    if (!compute_world_skinned_mesh_bounds(model_path, world_transform, center, radius)) {
        return true;
    }
    return frustum.contains_sphere(center, radius);
}

// ---------------------------------------------------------------------------
// Skybox
// ---------------------------------------------------------------------------
bool RenderPipeline::set_skybox(const std::array<std::string, 6>& face_paths) {
    if (!ctx_) return false;

    clear_skybox();

    // 1. 加载六个面的 CPU 数据（全部要求同尺寸同通道）
    const assets::TextureData* faces[6] = {};
    for (int i = 0; i < 6; ++i) {
        auto handle = assets::AssetManager::instance().load<assets::TextureData>(face_paths[i]);
        if (!handle.valid()) {
            GLOG_ERROR("RenderPipeline: failed to load skybox face '{}'", face_paths[i]);
            return false;
        }
        faces[i] = handle.get();
        if (i > 0 && (faces[i]->width != faces[0]->width || faces[i]->height != faces[0]->height ||
                      faces[i]->channels != faces[0]->channels)) {
            GLOG_ERROR("RenderPipeline: skybox faces must have identical size/channels");
            return false;
        }
    }

    // 2. 上传 cubemap（必须在 RenderContext::start() 之前，主线程持有 GPU context）
    skybox_texture_ = ctx_->create_texture();
    ITexture* tex_ptr = ctx_->texture(skybox_texture_);
    if (!skybox_texture_.is_valid() || !tex_ptr) return false;
    const void* face_data[6] = {};
    for (int i = 0; i < 6; ++i) face_data[i] = faces[i]->data();
    if (!tex_ptr->upload_cubemap(face_data, faces[0]->width, faces[0]->height, faces[0]->channels)) {
        GLOG_ERROR("RenderPipeline: failed to upload skybox cubemap");
        clear_skybox();
        return false;
    }

    // 3. 缓存线性 radiance（sRGB→linear，RGBA32F），供
    //    set_environment_from_skybox 派生 IBL（无独立 HDR 环境时）。
    skybox_radiance_size_ = faces[0]->width;
    for (int i = 0; i < 6; ++i) {
        const int w = faces[i]->width;
        const int h = faces[i]->height;
        const int ch = faces[i]->channels;
        const unsigned char* src = static_cast<const unsigned char*>(faces[i]->data());
        auto& dst = skybox_radiance_faces_[static_cast<size_t>(i)];
        dst.assign(static_cast<size_t>(w) * h * 4, 0.0f);
        for (int p = 0; p < w * h; ++p) {
            auto lin = [](unsigned char c) {
                const float v = c / 255.0f;
                return v <= 0.04045f ? v / 12.92f
                                     : std::pow((v + 0.055f) / 1.055f, 2.4f);
            };
            dst[static_cast<size_t>(p) * 4 + 0] = lin(src[static_cast<size_t>(p) * ch + 0]);
            dst[static_cast<size_t>(p) * 4 + 1] = lin(src[static_cast<size_t>(p) * ch + 1]);
            dst[static_cast<size_t>(p) * 4 + 2] = lin(src[static_cast<size_t>(p) * ch + 2]);
            dst[static_cast<size_t>(p) * 4 + 3] = 1.0f;
        }
    }

    // 4. 加载 skybox shader（Vulkan 走专用管线变体）
    {
        RHIShaderHandle shader = ctx_->create_shader();
        IShader* shader_ptr = ctx_->shader(shader);
        IFramebuffer* target_ptr = hdr_enabled_ ? ctx_->framebuffer(hdr_fbo_) : nullptr;
        if (!shader.is_valid() || !shader_ptr ||
            !shader_ptr->load_program("skybox", shader_dir_, target_ptr, true, false, true)) {
            GLOG_ERROR("RenderPipeline: failed to load skybox shader");
            clear_skybox();
            return false;
        }
        skybox_shader_ = shader;
        owns_shaders_ = true;
    }

    if (!create_skybox_mesh(ctx_)) {
        clear_skybox();
        return false;
    }

    // 记录路径，供 hot_reload() 重建后恢复天空盒
    skybox_paths_ = face_paths;
    skybox_set_ = true;
    GLOG_INFO("RenderPipeline: skybox set ({} faces, {}x{})", 6, faces[0]->width, faces[0]->height);
    return true;
}

void RenderPipeline::clear_skybox() {
    if (!ctx_) return;
    skybox_set_ = false;
    skybox_paths_ = {};
    skybox_radiance_size_ = 0;
    for (auto& f : skybox_radiance_faces_) f.clear();
    if (skybox_mesh_.is_valid()) {
        ctx_->destroy_mesh(skybox_mesh_);
        skybox_mesh_ = RHIMeshHandle{};
    }
    if (skybox_shader_.is_valid()) {
        ctx_->destroy_shader(skybox_shader_);
        skybox_shader_ = RHIShaderHandle{};
    }
    if (skybox_texture_.is_valid()) {
        ctx_->destroy_texture(skybox_texture_);
        skybox_texture_ = RHITextureHandle{};
    }
}

bool RenderPipeline::set_environment_hdr(const std::string& hdr_path) {
    if (!ctx_) return false;

    clear_environment();
    if (hdr_path.empty()) {
        return true;
    }

    auto tex_data = assets::AssetManager::instance().load<assets::TextureData>(hdr_path);
    if (!tex_data.valid() || !tex_data.get()) {
        GLOG_ERROR("RenderPipeline: failed to load HDR environment '{}'", hdr_path);
        return false;
    }

    const assets::TextureData* data = tex_data.get();
    if (!data->is_float || data->float_pixels.empty()) {
        GLOG_ERROR("RenderPipeline: environment '{}' is not HDR float data", hdr_path);
        return false;
    }

    // 生成 IBL 数据（CPU 端）
    auto ibl = IBLGenerator::generate(data, 512, 32, 128, 256);
    if (!ibl || !ibl->valid()) {
        GLOG_ERROR("RenderPipeline: failed to generate IBL from '{}'", hdr_path);
        return false;
    }

    if (!upload_ibl_data(*ibl)) {
        clear_environment();
        return false;
    }

    // 记录路径，供 hot_reload() 重建后恢复 HDR 环境
    environment_hdr_path_ = hdr_path;
    environment_set_ = true;
    environment_from_skybox_ = false;
    GLOG_INFO("RenderPipeline: HDR environment set '{}' ({}x{})", hdr_path, data->width, data->height);
    return true;
}

bool RenderPipeline::upload_ibl_data(const IBLData& ibl) {
    if (!ctx_) return false;

    // 上传 radiance cubemap（同时用作天空盒回退）
    {
        const void* faces[6] = {};
        for (int i = 0; i < 6; ++i) faces[i] = ibl.radiance_faces[i].data();
        ibl_radiance_texture_ = ctx_->create_texture();
        ITexture* tex = ctx_->texture(ibl_radiance_texture_);
        if (!ibl_radiance_texture_.is_valid() || !tex ||
            !tex->upload_cubemap_hdr(faces, ibl.cubemap_size, ibl.cubemap_size)) {
            GLOG_ERROR("RenderPipeline: failed to upload radiance cubemap");
            return false;
        }
    }

    // 上传 irradiance cubemap
    {
        const void* faces[6] = {};
        for (int i = 0; i < 6; ++i) faces[i] = ibl.irradiance_faces[i].data();
        ibl_irradiance_texture_ = ctx_->create_texture();
        ITexture* tex = ctx_->texture(ibl_irradiance_texture_);
        if (!ibl_irradiance_texture_.is_valid() || !tex ||
            !tex->upload_cubemap_hdr(faces, ibl.irradiance_size, ibl.irradiance_size)) {
            GLOG_ERROR("RenderPipeline: failed to upload irradiance cubemap");
            return false;
        }
    }

    // 上传 prefilter cubemap（多级 mip，shader 按 roughness 采样）
    {
        std::array<const void*, 6> level_faces[IBLData::k_prefilter_mip_levels]{};
        const void* mip_ptrs[IBLData::k_prefilter_mip_levels] = {};
        const int levels = static_cast<int>(ibl.prefilter_mips.size());
        if (levels <= 0 || levels > IBLData::k_prefilter_mip_levels) {
            GLOG_ERROR("RenderPipeline: invalid prefilter mip count {}", levels);
            return false;
        }
        for (int l = 0; l < levels; ++l) {
            for (int i = 0; i < 6; ++i) {
                level_faces[static_cast<size_t>(l)][i] = ibl.prefilter_mips[static_cast<size_t>(l)][i].data();
            }
            mip_ptrs[static_cast<size_t>(l)] = level_faces[static_cast<size_t>(l)].data();
        }
        ibl_prefilter_texture_ = ctx_->create_texture();
        ITexture* tex = ctx_->texture(ibl_prefilter_texture_);
        if (!ibl_prefilter_texture_.is_valid() || !tex ||
            !tex->upload_cubemap_hdr_mips(mip_ptrs, levels, ibl.prefilter_size, ibl.prefilter_size)) {
            GLOG_ERROR("RenderPipeline: failed to upload prefilter cubemap");
            return false;
        }
    }

    // 上传 BRDF LUT（2D，RG 扩展为 RGBA16F）
    {
        std::vector<float> rgba_lut;
        rgba_lut.resize(static_cast<size_t>(ibl.brdf_size) * ibl.brdf_size * 4);
        for (int y = 0; y < ibl.brdf_size; ++y) {
            for (int x = 0; x < ibl.brdf_size; ++x) {
                size_t src = (static_cast<size_t>(y) * ibl.brdf_size + x) * 2;
                size_t dst = (static_cast<size_t>(y) * ibl.brdf_size + x) * 4;
                rgba_lut[dst + 0] = ibl.brdf_lut[src + 0];
                rgba_lut[dst + 1] = ibl.brdf_lut[src + 1];
                rgba_lut[dst + 2] = 0.0f;
                rgba_lut[dst + 3] = 1.0f;
            }
        }
        ibl_brdf_lut_texture_ = ctx_->create_texture();
        ITexture* tex = ctx_->texture(ibl_brdf_lut_texture_);
        if (!ibl_brdf_lut_texture_.is_valid() || !tex ||
            !tex->upload_data(rgba_lut.data(), ibl.brdf_size, ibl.brdf_size, 4)) {
            GLOG_ERROR("RenderPipeline: failed to upload BRDF LUT");
            return false;
        }
        tex->set_filter(TextureFilter::Linear, TextureFilter::Linear);
        tex->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
    }
    return true;
}

bool RenderPipeline::set_environment_from_skybox() {
    if (!ctx_ || skybox_radiance_size_ <= 0) return false;

    auto ibl = IBLGenerator::generate_from_cubemap(skybox_radiance_faces_, skybox_radiance_size_,
                                                   32, 128, 256);
    if (!ibl || !ibl->valid()) {
        GLOG_ERROR("RenderPipeline: failed to generate IBL from skybox");
        return false;
    }
    clear_environment();
    if (!upload_ibl_data(*ibl)) {
        clear_environment();
        return false;
    }
    // 记录来源，供 hot_reload() 重建后从天空盒重新派生 IBL
    environment_hdr_path_.clear();
    environment_set_ = true;
    environment_from_skybox_ = true;
    GLOG_INFO("RenderPipeline: environment derived from skybox ({}x{})", skybox_radiance_size_,
              skybox_radiance_size_);
    return true;
}

void RenderPipeline::clear_environment() {
    if (!ctx_) return;
    environment_set_ = false;
    environment_hdr_path_.clear();
    environment_from_skybox_ = false;
    if (ibl_radiance_texture_.is_valid()) {
        ctx_->destroy_texture(ibl_radiance_texture_);
        ibl_radiance_texture_ = RHITextureHandle{};
    }
    if (ibl_irradiance_texture_.is_valid()) {
        ctx_->destroy_texture(ibl_irradiance_texture_);
        ibl_irradiance_texture_ = RHITextureHandle{};
    }
    if (ibl_prefilter_texture_.is_valid()) {
        ctx_->destroy_texture(ibl_prefilter_texture_);
        ibl_prefilter_texture_ = RHITextureHandle{};
    }
    if (ibl_brdf_lut_texture_.is_valid()) {
        ctx_->destroy_texture(ibl_brdf_lut_texture_);
        ibl_brdf_lut_texture_ = RHITextureHandle{};
    }
}

bool RenderPipeline::create_skybox_mesh(RenderContext* ctx) {
    // 单位立方体（36 顶点），顶点布局与 MeshRenderer 一致（56 字节），
    // 只有 position 有意义，skybox shader 采样方向即顶点坐标。
    struct VertexGPU {
        float x, y, z;
        float nx, ny, nz;
        float tx, ty, tz;
        float u, v;
        float r, g, b;
    };
    const float p = 1.0f;
    const float n = -1.0f;
    const math::Vector3f positions[36] = {
        {n, n, p}, {p, p, p}, {p, n, p}, {p, p, p}, {n, n, p}, {n, p, p}, // front
        {p, n, n}, {n, p, n}, {n, n, n}, {n, p, n}, {p, n, n}, {p, p, n}, // back
        {n, n, n}, {n, n, p}, {n, p, p}, {n, p, p}, {n, p, n}, {n, n, n}, // left
        {p, n, p}, {p, n, n}, {p, p, n}, {p, p, n}, {p, p, p}, {p, n, p}, // right
        {n, p, p}, {p, p, p}, {p, p, n}, {p, p, n}, {n, p, n}, {n, p, p}, // top
        {n, n, n}, {p, n, n}, {p, n, p}, {p, n, p}, {n, n, p}, {n, n, n}, // bottom
    };

    std::vector<VertexGPU> verts(36);
    for (int i = 0; i < 36; ++i) {
        verts[i] = {positions[i].x, positions[i].y, positions[i].z,
                    0, 1, 0, 1, 0, 0, 0, 0, 1, 1, 1};
    }

    skybox_mesh_ = ctx->create_mesh();
    IMesh* mesh_ptr = ctx->mesh(skybox_mesh_);
    if (!skybox_mesh_.is_valid() || !mesh_ptr) return false;
    mesh_ptr->upload_vertices(verts.data(),
                              static_cast<uint32_t>(verts.size() * sizeof(VertexGPU)),
                              static_cast<uint32_t>(verts.size()));
    VertexLayout layout;
    layout.stride = sizeof(VertexGPU);
    layout.attributes = {
        {0, VertexType::Float3, false, 0},
        {1, VertexType::Float3, false, 3 * sizeof(float)},
        {2, VertexType::Float3, false, 6 * sizeof(float)},
        {3, VertexType::Float2, false, 9 * sizeof(float)},
        {4, VertexType::Float3, false, 11 * sizeof(float)}
    };
    mesh_ptr->set_layout(layout);
    return true;
}

void RenderPipeline::render_skybox(RenderContext& ctx) {
    if (!camera_) return;
    RHITextureHandle skybox_tex = skybox_texture_.is_valid() ? skybox_texture_ : ibl_radiance_texture_;
    if (!skybox_tex.is_valid() || !skybox_shader_.is_valid() || !skybox_mesh_.is_valid()) {
        return;
    }

    // 天空盒：关深度测试/写入、关剔除（从立方体内部观察），画完后恢复
    ctx.set_depth_test(false);
    ctx.set_depth_write(false);
    ctx.set_cull_face(CullMode::None);
    ctx.set_blend(false);

    // 去掉 view 的平移分量，让天空盒始终跟随相机
    math::Matrix4f view = camera_->get_view_matrix();
    view(0, 3) = 0.0f;
    view(1, 3) = 0.0f;
    view(2, 3) = 0.0f;

    ctx.set_shader(skybox_shader_);
    ctx.set_uniform_mat4(skybox_shader_, "uView", view);
    ctx.set_uniform_mat4(skybox_shader_, "uProjection", get_projection_matrix());

    ITexture* tex_ptr = ctx_->texture(skybox_tex);
    if (tex_ptr) {
        tex_ptr->bind(TextureSlots::kSkyboxCube);
    }
    ctx.set_texture(skybox_shader_, skybox_tex, TextureSlots::kSkyboxCube, "");
    ctx.set_uniform_int(skybox_shader_, "uSkybox", TextureSlots::kSkyboxCube);

    ctx.draw_mesh(skybox_mesh_, skybox_shader_);

    ctx.set_depth_test(true);
    ctx.set_depth_write(true);
    ctx.set_cull_face(cull_disabled_ ? CullMode::None : CullMode::Back);
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------
void RenderPipeline::render_scene(scene::Scene& scene, RenderContext& ctx) {
    if (!initialized_ || !camera_) return;

    // ---- SDFGI 更新（每帧，在场景渲染之前） ----
    if (sdfgi_enabled_ && sdfgi_.valid()) {
        // 收集方向光方向与颜色
        std::vector<math::Vector3f> light_dirs;
        std::vector<math::Vector3f> light_colors;
        for (const auto& light : lights_) {
            if (light.type == LightType::Directional) {
                light_dirs.push_back(light.direction.normalized());
                light_colors.push_back(light.color * light.intensity);
            }
        }
        sdfgi_.update(camera_->position(), light_dirs, light_colors);
    }

    // TAA 抖动序列推进（投影矩阵每帧取不同子像素偏移）
    if (pp_params_.taa_enabled != 0) {
        ++taa_frame_;
    }

    // 每帧重置材质 bind 缓存（跨 pass 不保留）
    last_bound_material_pbr_ = nullptr;
    last_bound_material_skinned_ = nullptr;

    update_light_space_matrix();
    const bool render_shadow = shadow_enabled_ && shadow_light_index_ >= 0;

    // 相机视锥体：用于 forward pass 剔除不可见物体。
    // 必须用 GL 风格 NDC z∈[-1,1] 的投影矩阵做剔除：Vulkan 后端
    // get_projection_matrix() 会把 z 行重映射到 [0,1]，而 extract_frustum 的
    // near/far 平面公式（row3±row2）只对 [-1,1] 成立，否则 VK 上 near 平面
    // 落在 z=-1（永远不剔除近侧物体），视锥外的物体被多画出来。
    const math::Matrix4f camera_vp = camera_->get_projection_matrix() * camera_->get_view_matrix();
    const Frustum camera_frustum = extract_frustum(camera_vp);

    // 1. Shadow pass（仅第一个方向光；逐级联渲染，每级独立剔除/分辨率/bias）
    if (render_shadow) {
        if (shadow_atlas_enabled_ && shadow_atlas_.valid() && shadow_atlas_shader_.is_valid()) {
            // ---- Shadow Atlas 模式：所有级联渲染到同一张图集纹理 ----
            shadow_atlas_.free_all();

            // 为每个级联分配 atlas slot
            int slot_indices[k_max_cascades];
            for (int c = 0; c < cascade_count_; ++c) {
                slot_indices[c] = shadow_atlas_.allocate(cascade_sizes_[c], (uint32_t)c);
                if (slot_indices[c] < 0) {
                    GLOG_WARN("ShadowAtlas: failed to allocate cascade {} slot", c);
                }
            }

            // 绑定 atlas FBO 并清空整张图集深度
            ctx.set_framebuffer(shadow_atlas_.atlas_fbo());
            ctx.set_viewport(0, 0, shadow_atlas_.atlas_size(), shadow_atlas_.atlas_size());
            ctx.clear_depth();
            ctx.set_depth_test(true);
            ctx.set_depth_write(true);
            ctx.set_cull_face(cull_disabled_ ? CullMode::None : CullMode::Back);

            for (int c = 0; c < cascade_count_; ++c) {
                if (slot_indices[c] < 0) continue;

                const auto& slot = shadow_atlas_.get_slot(slot_indices[c]);
                ctx.set_viewport(slot.x, slot.y, slot.size, slot.size);
                ctx.set_shader(shadow_atlas_shader_);

                // Normal Offset Shadow Mapping
                ctx.set_uniform_float(shadow_atlas_shader_, "uNormalOffset",
                                      cascade_texel_sizes_[c] * normal_offset_scale_);
                // Atlas UV 变换：将 NDC 映射到 atlas slot 区域
                math::Vector4f uv_xform = shadow_atlas_.slot_uv_transform(slot_indices[c]);
                ctx.set_uniform_vec4(shadow_atlas_shader_, "uAtlasOffset", uv_xform);

                const Frustum shadow_frustum = extract_frustum(cascade_light_space_matrices_[c]);
                ecs::foreach_with_components<components::MeshRenderer, components::Transform>(
                    scene,
                    [&](scene::Entity* entity, components::MeshRenderer* mr, components::Transform* /*transform*/) {
                        if (!mr->enabled || mr->mesh_path.empty() || !mr->gpu_mesh_handle().is_valid()) return;
                        const math::Matrix4f& model = entity->world_transform();
                        if (!is_inside_frustum(shadow_frustum, model, mr->mesh_path)) return;
                        ctx.set_uniform_mat4(shadow_atlas_shader_, "uModel", model);
                        ctx.set_uniform_mat4(shadow_atlas_shader_, "uLightSpaceMatrix",
                                             cascade_light_space_matrices_[c]);
                        // [Alpha Test]
                        if (mr->material && mr->material->albedo_texture().is_valid()) {
                            ctx.set_texture(shadow_atlas_shader_, mr->material->albedo_texture(),
                                            TextureSlots::kPBRAlbedo, "");
                            ctx.set_uniform_int(shadow_atlas_shader_, "uAlbedoMap", TextureSlots::kPBRAlbedo);
                            ctx.set_uniform_int(shadow_atlas_shader_, "uUseAlbedoMap", 1);
                        } else {
                            ctx.set_uniform_int(shadow_atlas_shader_, "uUseAlbedoMap", 0);
                        }
                        ctx.draw_mesh(mr->gpu_mesh_handle(), shadow_atlas_shader_);
                    });
            }
            ctx.set_framebuffer(RHIFramebufferHandle{});
        } else {
            // ---- 传统模式：每级联独立渲染 ----
            for (int c = 0; c < cascade_count_; ++c) {
                const Frustum shadow_frustum = extract_frustum(cascade_light_space_matrices_[c]);
                begin_shadow_pass(ctx, c);

                // 根据 shadow_mode_ 选择正确的 shader 句柄
                RHIShaderHandle current_shadow = shadow_shader_;
                if (shadow_mode_ == ShadowMode::VSM) {
                    current_shadow = shadow_vsm_shader_;
                } else if (shadow_mode_ == ShadowMode::ESM) {
                    current_shadow = shadow_esm_shader_;
                }

                ecs::foreach_with_components<components::MeshRenderer, components::Transform>(
                    scene,
                    [&](scene::Entity* entity, components::MeshRenderer* mr, components::Transform* /*transform*/) {
                        if (!mr->enabled || mr->mesh_path.empty() || !mr->gpu_mesh_handle().is_valid()) return;
                        const math::Matrix4f& model = entity->world_transform();
                        if (!is_inside_frustum(shadow_frustum, model, mr->mesh_path)) return;
                        ctx.set_uniform_mat4(current_shadow, "uModel", model);
                        ctx.set_uniform_mat4(current_shadow, "uLightSpaceMatrix",
                                             cascade_light_space_matrices_[c]);
                        // [Alpha Test] 绑定材质 albedo 纹理以供 alpha discard
                        if (mr->material && mr->material->albedo_texture().is_valid()) {
                            ctx.set_texture(current_shadow, mr->material->albedo_texture(),
                                            TextureSlots::kPBRAlbedo, "");
                            ctx.set_uniform_int(current_shadow, "uAlbedoMap", TextureSlots::kPBRAlbedo);
                            ctx.set_uniform_int(current_shadow, "uUseAlbedoMap", 1);
                        } else {
                            ctx.set_uniform_int(current_shadow, "uUseAlbedoMap", 0);
                        }
                        // ESM 额外参数
                        if (shadow_mode_ == ShadowMode::ESM) {
                            ctx.set_uniform_float(current_shadow, "uESMExponent", esm_exponent_);
                        }
                        ctx.draw_mesh(mr->gpu_mesh_handle(), current_shadow);
                    });
                end_shadow_pass(ctx);
            }

            // VSM 模式下对每级级联做 blur 以减少 light bleeding
            if (shadow_mode_ == ShadowMode::VSM) {
                render_vsm_blur(ctx);
            }
        }
    }

    // 1b. 点光源双抛物面阴影（在 CSM 之后、GBuffer/Forward pass 之前）
    render_point_shadows(ctx, scene);

    // 2. Forward PBR pass or Deferred GBuffer pass (HDR target or backbuffer)
    if (deferred_enabled_ && hdr_enabled_) {
        // Deferred Rendering Path
        // 2a. GBuffer pass: render scene to MRT
        begin_gbuffer_pass(ctx);
        render_skybox(ctx);
        render_grid(ctx);
        bind_global_uniforms(ctx);
        // 收集绘制项（同 forward）
        last_bound_material_pbr_ = nullptr;
        last_bound_material_skinned_ = nullptr;
        auto& opaque_items = opaque_items_;
        auto& transparent_items = transparent_items_;
        auto& viewmodel_items = viewmodel_items_;
        auto& skinned_opaque_items = skinned_opaque_items_;
        auto& skinned_transparent_items = skinned_transparent_items_;
        opaque_items.clear();
        transparent_items.clear();
        viewmodel_items.clear();
        skinned_opaque_items.clear();
        skinned_transparent_items.clear();
        const math::Vector3f cam_pos = camera_->position();
        ecs::foreach_with_components<components::MeshRenderer, components::Transform>(
            scene,
            [&](scene::Entity* entity, components::MeshRenderer* mr, components::Transform* /*transform*/) {
                if (!mr->enabled || mr->mesh_path.empty() || !mr->gpu_mesh_handle().is_valid()) return;
                math::Matrix4f model = entity->world_transform();
                if (mr->billboard && camera_) {
                    model = math::billboard_matrix(
                        math::Vector3f(model(0, 3), model(1, 3), model(2, 3)),
                        entity->transform()->scale,
                        camera_->position());
                }
                if (!is_inside_frustum(camera_frustum, model, mr->mesh_path)) return;
                const Material* mat = mr->material.get();
                const bool transparent = mat && mat->blend_mode == Material::BlendMode::Blend;
                math::Vector3f pos(model(0, 3), model(1, 3), model(2, 3));
                float dist_sq = (pos - cam_pos).length_sq();
                DrawItem item{mr->gpu_mesh_handle(), mat, model, dist_sq};
                if (!mr->depth_test) {
                    viewmodel_items.push_back(item);
                } else if (transparent) {
                    transparent_items.push_back(item);
                } else {
                    opaque_items.push_back(item);
                }
            });
        // 蒙皮网格
        if (skinned_pbr_shader_.is_valid()) {
            ecs::foreach_with_components<components::SkinnedMeshRenderer, components::Transform>(
                scene,
                [&](scene::Entity* entity, components::SkinnedMeshRenderer* mr, components::Transform* /*transform*/) {
                    if (!mr->enabled || mr->model_path.empty() || !mr->gpu_mesh_handle().is_valid() || !mr->palette()) return;
                    const math::Matrix4f& model = entity->world_transform();
                    if (!is_inside_frustum_skinned(camera_frustum, model, mr->model_path)) return;
                    const Material* mat = mr->material.get();
                    const bool transparent = mat && mat->blend_mode == Material::BlendMode::Blend;
                    math::Vector3f pos(model(0, 3), model(1, 3), model(2, 3));
                    float dist_sq = (pos - cam_pos).length_sq();
                    SkinnedDrawItem item{mr->gpu_mesh_handle(), mat, model, mr->palette(), dist_sq};
                    if (transparent) {
                        skinned_transparent_items.push_back(std::move(item));
                    } else {
                        skinned_opaque_items.push_back(std::move(item));
                    }
                });
        }
        // GBuffer 不透明物体（使用 g_buffer shader）
        ctx.set_shader(gbuffer_shader_);
        ctx.set_blend(false);
        ctx.set_depth_write(true);
        for (const auto& item : opaque_items) {
            render_mesh_to_gbuffer(item.mesh, item.material, item.model, ctx);
        }
        for (const auto& item : skinned_opaque_items) {
            render_skinned_mesh_to_gbuffer(item.mesh, item.material, item.model, item.palette, ctx);
        }
        end_gbuffer_pass(ctx);

        // 2b. Deferred Lighting Pass: full-screen quad, read GBuffer, write HDR
        begin_deferred_lighting_pass(ctx);
        render_deferred_lighting_to_hdr(ctx);
        end_deferred_lighting_pass(ctx);

        // 2b'. Decal 贴花（Deferred Lighting 后、透明前，blend 合成到 HDR 颜色）
        if (decal_enabled_ && decal_shader_.is_valid() && decal_box_mesh_.is_valid()) {
            render_decal_deferred(ctx);
        }

        // 2c. 透明物体：使用 forward PBR 在 HDR target 上叠层
        if (!transparent_items.empty() || !skinned_transparent_items.empty()) {
            std::sort(transparent_items.begin(), transparent_items.end(),
                      [](const DrawItem& a, const DrawItem& b) { return a.dist_sq > b.dist_sq; });
            std::sort(skinned_transparent_items.begin(), skinned_transparent_items.end(),
                      [](const SkinnedDrawItem& a, const SkinnedDrawItem& b) { return a.dist_sq > b.dist_sq; });
            ctx.set_framebuffer(hdr_fbo_);
            ctx.set_viewport(0, 0, viewport_width_, viewport_height_);
            ctx.set_blend(true);
            ctx.set_depth_write(false);
            ctx.set_depth_test(true);
            ctx.set_cull_face(cull_disabled_ ? CullMode::None : CullMode::Back);
            ctx.set_shader(pbr_shader_);
            for (const auto& item : transparent_items) {
                render_mesh_internal(item.mesh, item.material, item.model, ctx);
            }
            for (const auto& item : skinned_transparent_items) {
                render_skinned_mesh_internal(item.mesh, item.material, item.model, item.palette, ctx);
            }
            ctx.set_blend(false);
            ctx.set_depth_write(true);
        }
        // 2d. Viewmodel（FPS 武器）
        if (!viewmodel_items.empty()) {
            ctx.set_framebuffer(hdr_fbo_);
            ctx.set_viewport(0, 0, viewport_width_, viewport_height_);
            ctx.set_blend(false);
            ctx.set_depth_test(false);
            ctx.set_depth_write(false);
            ctx.set_cull_face(CullMode::None);
            ctx.set_shader(pbr_shader_);
            for (const auto& item : viewmodel_items) {
                render_mesh_internal(item.mesh, item.material, item.model, ctx);
            }
            ctx.set_depth_test(true);
            ctx.set_depth_write(true);
        }

        // ---- 水面渲染（透明物体后，SSR 前，Deferred 路径） ----
        if (water_enabled_ && water_.valid() && camera_ && hdr_fbo_.is_valid()) {
            water_time_ += 0.016f;

            const float water_y = water_height_;
            math::Vector3f ref_cam_pos = camera_->position();
            ref_cam_pos.y = 2.0f * water_y - ref_cam_pos.y;

            water_.render_reflection(&ctx, ref_cam_pos, water_y);

            ctx.set_framebuffer(hdr_fbo_);
            ctx.set_viewport(0, 0, viewport_width_, viewport_height_);

            math::Matrix4f view_proj = get_projection_matrix() * camera_->get_view_matrix();
            water_.render_water(&ctx,
                                water_.reflection_tex(),
                                hdr_color_,
                                hdr_depth_,
                                view_proj,
                                camera_->position(),
                                water_time_,
                                water_height_);

            ctx.set_framebuffer(hdr_fbo_);
            ctx.set_viewport(0, 0, viewport_width_, viewport_height_);
        }

        // SSR 屏幕空间反射（HDR 渲染后、SSIL/SSAO 前）
        if (ssr_enabled_ && ssr_.valid()) {
            ssr_.render(&ctx, hdr_color_, hdr_depth_, gbuffer_normal_roughness_,
                        pp_params_, viewport_width_, viewport_height_);
        }

        // SSIL 屏幕空间间接光照（SSR 后、SSAO 前）
        if (ssil_enabled_ && ssil_.valid()) {
            ssil_.render(&ctx, hdr_color_, hdr_depth_, gbuffer_normal_roughness_,
                         pp_params_, viewport_width_, viewport_height_);
        }

        // 体积雾（PBR/Deferred 渲染后、后处理前）
        if (fog_enabled_ && fog_.valid() && camera_) {
            math::Matrix4f inv_vp = (get_projection_matrix() * camera_->get_view_matrix()).inverse();
            fog_.render(&ctx, hdr_depth_, inv_vp, camera_->get_view_matrix(),
                        camera_->position(), fog_color_, fog_density_, fog_height_,
                        camera_->near_plane(), camera_->far_plane());
            fog_.render_apply(&ctx, hdr_color_, hdr_depth_, inv_vp, camera_->position());
        }

        // 后处理：SSAO, Contact Shadow, Bloom, DOF, Motion Blur, Auto Exposure, TAA, Tonemap
        render_ssao(ctx);
        render_contact_shadow(ctx);
        render_bloom(ctx);
        // DOF
        if (dof_enabled_ && dof_.valid()) {
            dof_.render(&ctx, hdr_color_, hdr_depth_, pp_params_, viewport_width_, viewport_height_);
            if (dof_.dof_tex().is_valid()) {
                hdr_color_ = dof_.dof_tex();
            }
        }
        // Motion Blur
        if (motion_blur_enabled_ && motion_blur_.valid()) {
            motion_blur_.render(&ctx, hdr_color_, hdr_depth_, RHITextureHandle{}, pp_params_, viewport_width_, viewport_height_);
            if (motion_blur_.output_tex().is_valid()) {
                hdr_color_ = motion_blur_.output_tex();
            }
        }
        render_auto_exposure(ctx);
        render_taa(ctx);
        render_tonemap(ctx);
    } else if (hdr_enabled_) {
        begin_hdr_forward_pass(ctx);
    } else {
        begin_forward_pass(ctx);
    }

    // 2a. Skybox（最先绘制，作为背景）
    render_skybox(ctx);

    // 2a'. Scene View 网格线（skybox 之后，场景物体之前）
    render_grid(ctx);

    bind_global_uniforms(ctx);

    // 2b. 收集绘制项并拆分不透明 / 透明（同时做相机视锥体剔除）
    // 容器为成员变量，每帧 clear() 复用 capacity，避免反复堆分配。
    auto& opaque_items = opaque_items_;
    auto& transparent_items = transparent_items_;
    auto& viewmodel_items = viewmodel_items_;
    auto& skinned_opaque_items = skinned_opaque_items_;
    auto& skinned_transparent_items = skinned_transparent_items_;
    opaque_items.clear();
    transparent_items.clear();
    viewmodel_items.clear();
    skinned_opaque_items.clear();
    skinned_transparent_items.clear();
    const math::Vector3f cam_pos = camera_->position();
    ecs::foreach_with_components<components::MeshRenderer, components::Transform>(
        scene,
        [&](scene::Entity* entity, components::MeshRenderer* mr, components::Transform* /*transform*/) {
            if (!mr->enabled || mr->mesh_path.empty() || !mr->gpu_mesh_handle().is_valid()) return;
            math::Matrix4f model = entity->world_transform();
            if (mr->billboard && camera_) {
                // Sprite3D 广告牌：忽略自身旋转，局部 +Z 朝向相机
                model = math::billboard_matrix(
                    math::Vector3f(model(0, 3), model(1, 3), model(2, 3)),
                    entity->transform()->scale,
                    camera_->position());
            }
            if (!is_inside_frustum(camera_frustum, model, mr->mesh_path)) return;
            const Material* mat = mr->material.get();
            const bool transparent = mat && mat->blend_mode == Material::BlendMode::Blend;
            math::Vector3f pos(model(0, 3), model(1, 3), model(2, 3));
            float dist_sq = (pos - cam_pos).length_sq();
            DrawItem item{mr->gpu_mesh_handle(), mat, model, dist_sq};
            if (!mr->depth_test) {
                viewmodel_items.push_back(item);
            } else if (transparent) {
                transparent_items.push_back(item);
            } else {
                opaque_items.push_back(item);
            }
        });

    // 蒙皮网格：skinned 管线可用且 palette 已就绪才绘制
    if (skinned_pbr_shader_.is_valid()) {
        ecs::foreach_with_components<components::SkinnedMeshRenderer, components::Transform>(
            scene,
            [&](scene::Entity* entity, components::SkinnedMeshRenderer* mr, components::Transform* /*transform*/) {
                if (!mr->enabled || mr->model_path.empty() || !mr->gpu_mesh_handle().is_valid() || !mr->palette()) return;
                const math::Matrix4f& model = entity->world_transform();
                if (!is_inside_frustum_skinned(camera_frustum, model, mr->model_path)) return;
                const Material* mat = mr->material.get();
                const bool transparent = mat && mat->blend_mode == Material::BlendMode::Blend;
                math::Vector3f pos(model(0, 3), model(1, 3), model(2, 3));
                float dist_sq = (pos - cam_pos).length_sq();
                SkinnedDrawItem item{mr->gpu_mesh_handle(), mat, model, mr->palette(), dist_sq};
                if (transparent) {
                    skinned_transparent_items.push_back(std::move(item));
                } else {
                    skinned_opaque_items.push_back(std::move(item));
                }
            });
    }

    // 2c. 不透明物体：blend 关、深度写开
    ctx.set_blend(false);
    ctx.set_depth_write(true);
    for (const auto& item : opaque_items) {
        render_mesh_internal(item.mesh, item.material, item.model, ctx);
    }
    for (const auto& item : skinned_opaque_items) {
        render_skinned_mesh_internal(item.mesh, item.material, item.model, item.palette, ctx);
    }

    // 2c'. Decal 贴花（不透明后、透明前，blend 合成到 HDR 颜色）
    if (decal_enabled_ && decal_shader_.is_valid() && decal_box_mesh_.is_valid()) {
        render_decal_forward(ctx);
    }

    // 2d. 透明物体：按到相机距离从远到近排序，blend 开、深度写关
    if (!transparent_items.empty() || !skinned_transparent_items.empty()) {
        std::sort(transparent_items.begin(), transparent_items.end(),
                  [](const DrawItem& a, const DrawItem& b) { return a.dist_sq > b.dist_sq; });
        std::sort(skinned_transparent_items.begin(), skinned_transparent_items.end(),
                  [](const SkinnedDrawItem& a, const SkinnedDrawItem& b) { return a.dist_sq > b.dist_sq; });
        ctx.set_blend(true);
        ctx.set_depth_write(false);
        for (const auto& item : transparent_items) {
            render_mesh_internal(item.mesh, item.material, item.model, ctx);
        }
        for (const auto& item : skinned_transparent_items) {
            render_skinned_mesh_internal(item.mesh, item.material, item.model, item.palette, ctx);
        }
        ctx.set_blend(false);
        ctx.set_depth_write(true);
    }

    // 2e. Viewmodel（FPS 武器）：关闭深度测试/深度写，保证枪械不被墙壁遮挡。
    if (!viewmodel_items.empty()) {
        ctx.set_blend(false);
        ctx.set_depth_test(false);
        ctx.set_depth_write(false);
        for (const auto& item : viewmodel_items) {
            render_mesh_internal(item.mesh, item.material, item.model, ctx);
        }
        ctx.set_depth_test(true);
        ctx.set_depth_write(true);
    }

    if (hdr_enabled_) {
        end_hdr_forward_pass(ctx);

        // ---- 水面渲染（透明物体后，SSR 前） ----
        if (water_enabled_ && water_.valid() && camera_ && hdr_fbo_.is_valid()) {
            water_time_ += 0.016f; // 约 60fps 时间增量

            // 计算反射相机（镜面反射：翻转相机关于水面）
            const float water_y = water_height_;
            math::Vector3f ref_cam_pos = camera_->position();
            ref_cam_pos.y = 2.0f * water_y - ref_cam_pos.y;

            // 设置反射 FBO
            water_.render_reflection(&ctx, ref_cam_pos, water_y);

            // 反射场景渲染：使用原始场景的相机方向，但翻转 Y
            // 简化的反射渲染：使用当前 HDR 颜色作为折射纹理
            // 渲染水面网格到当前 HDR FBO（直接 alpha blend）
            math::Matrix4f view_proj = get_projection_matrix() * camera_->get_view_matrix();
            water_.render_water(&ctx,
                                water_.reflection_tex(),  // 反射纹理
                                hdr_color_,                // 折射纹理（使用场景颜色）
                                hdr_depth_,                // 深度纹理
                                view_proj,
                                camera_->position(),
                                water_time_,
                                water_height_);

            // 恢复 HDR FBO 状态
            ctx.set_framebuffer(hdr_fbo_);
            ctx.set_viewport(0, 0, viewport_width_, viewport_height_);
        }

        // SSR 屏幕空间反射（HDR 渲染后、SSIL/SSAO 前）
        if (ssr_enabled_ && ssr_.valid()) {
            ssr_.render(&ctx, hdr_color_, hdr_depth_, gbuffer_normal_roughness_,
                        pp_params_, viewport_width_, viewport_height_);
        }

        // 3. SSIL 屏幕空间间接光照（SSR 后、SSAO 前）
        if (ssil_enabled_ && ssil_.valid()) {
            RHITextureHandle normal_roughness = deferred_enabled_ ? gbuffer_normal_roughness_ : RHITextureHandle{};
            ssil_.render(&ctx, hdr_color_, hdr_depth_, normal_roughness,
                         pp_params_, viewport_width_, viewport_height_);
        }

        // 体积雾（PBR 渲染后、后处理前）
        if (fog_enabled_ && fog_.valid() && camera_) {
            math::Matrix4f inv_vp = (get_projection_matrix() * camera_->get_view_matrix()).inverse();
            fog_.render(&ctx, hdr_depth_, inv_vp, camera_->get_view_matrix(),
                        camera_->position(), fog_color_, fog_density_, fog_height_,
                        camera_->near_plane(), camera_->far_plane());
            fog_.render_apply(&ctx, hdr_color_, hdr_depth_, inv_vp, camera_->position());
        }

        // 3. 屏幕空间环境光遮蔽（深度 → GTAO → 模糊）
        render_ssao(ctx);
        // 3a. 屏幕空间接触阴影（补 Peter-Panning 脚底黑）
        render_contact_shadow(ctx);
        // 4. Bloom（阈值 → 降采样链 → 上采样合成）
        render_bloom(ctx);
        // 4a. DOF 景深（Bloom 后，TAA 前）
        if (dof_enabled_ && dof_.valid()) {
            dof_.render(&ctx, hdr_color_, hdr_depth_, pp_params_, viewport_width_, viewport_height_);
            if (dof_.dof_tex().is_valid()) {
                hdr_color_ = dof_.dof_tex();
            }
        }
        // 4b. Motion Blur 运动模糊（DOF 后，自动曝光前）
        if (motion_blur_enabled_ && motion_blur_.valid()) {
            motion_blur_.render(&ctx, hdr_color_, hdr_depth_, RHITextureHandle{}, pp_params_, viewport_width_, viewport_height_);
            if (motion_blur_.output_tex().is_valid()) {
                hdr_color_ = motion_blur_.output_tex();
            }
        }
        // 5. 自动曝光（GPU 亮度反馈，更新曝光纹理）
        render_auto_exposure(ctx);
        // 6. TAA（时域累积 + 抖动 + 邻域钳制）
        render_taa(ctx);
        // 7. Tone mapping pass to backbuffer
        render_tonemap(ctx);
    } else {
        end_forward_pass(ctx);
    }
}

void RenderPipeline::render_mesh(RHIMeshHandle mesh, const Material* material, const math::Matrix4f& model,
                                 RenderContext& ctx) {
    bind_per_frame_uniforms(ctx, pbr_shader_);
    render_mesh_internal(mesh, material, model, ctx);
}

void RenderPipeline::render_mesh_internal(RHIMeshHandle mesh, const Material* material, const math::Matrix4f& model,
                                          RenderContext& ctx) {
    if (!mesh.is_valid() || !pbr_shader_.is_valid() || !camera_) return;

    // 双面材质关闭背面剔除
    const bool two_sided = material && material->two_sided;
    ctx.set_cull_face((cull_disabled_ || two_sided) ? CullMode::None : CullMode::Back);

    ctx.set_shader(pbr_shader_);
    ctx.set_uniform_mat4(pbr_shader_, "uModel", model);
    // 每 draw 设置（material 可能为空，bind 有缓存跳过），供 shader 翻转背面法线
    ctx.set_uniform_int(pbr_shader_, "uTwoSided", two_sided ? 1 : 0);

    if (material && material != last_bound_material_pbr_) {
        material->bind(&ctx, pbr_shader_);
        last_bound_material_pbr_ = material;
    }

    // 当 probe 系统启用且有可用 probe 时，覆盖全局 IBL
    if (probe_system_enabled_ && probe_system_.probe_count() > 0) {
        math::Vector3f pos(model(0, 3), model(1, 3), model(2, 3));
        bind_probe_ibl(ctx, pbr_shader_, pos);
    }

    ctx.draw_mesh(mesh, pbr_shader_);
}

void RenderPipeline::render_skinned_mesh(RHIMeshHandle mesh, const Material* material, const math::Matrix4f& model,
                                         std::shared_ptr<const std::vector<math::Matrix4f>> palette,
                                         RenderContext& ctx) {
    bind_per_frame_uniforms(ctx, skinned_pbr_shader_);
    render_skinned_mesh_internal(mesh, material, model, palette, ctx);
}

void RenderPipeline::render_skinned_mesh_internal(RHIMeshHandle mesh, const Material* material,
                                                  const math::Matrix4f& model,
                                                  std::shared_ptr<const std::vector<math::Matrix4f>> palette,
                                                  RenderContext& ctx) {
    if (!mesh.is_valid() || !skinned_pbr_shader_.is_valid() || !camera_) return;

    // 双面材质关闭背面剔除
    const bool two_sided = material && material->two_sided;
    ctx.set_cull_face((cull_disabled_ || two_sided) ? CullMode::None : CullMode::Back);

    ctx.set_shader(skinned_pbr_shader_);
    ctx.set_uniform_mat4(skinned_pbr_shader_, "uModel", model);
    // 每 draw 设置（material 可能为空，bind 有缓存跳过），供 shader 翻转背面法线
    ctx.set_uniform_int(skinned_pbr_shader_, "uTwoSided", two_sided ? 1 : 0);

    // palette：shared_ptr 按值捕获进命令队列，渲染线程执行时数据仍有效
    if (palette && !palette->empty()) {
        ctx.set_uniform_mat4_array(skinned_pbr_shader_, "uBonePalette", std::move(palette));
    }

    if (material && material != last_bound_material_skinned_) {
        material->bind(&ctx, skinned_pbr_shader_);
        last_bound_material_skinned_ = material;
    }

    // 当 probe 系统启用且有可用 probe 时，覆盖全局 IBL
    if (probe_system_enabled_ && probe_system_.probe_count() > 0) {
        math::Vector3f pos(model(0, 3), model(1, 3), model(2, 3));
        bind_probe_ibl(ctx, skinned_pbr_shader_, pos);
    }

    ctx.draw_mesh(mesh, skinned_pbr_shader_);
}

// ---------------------------------------------------------------------------
// Direct submit 路径（不依赖 Scene/Entity）
// ---------------------------------------------------------------------------
void RenderPipeline::register_mesh_mapping(IMesh* ptr, RHIMeshHandle handle) {
    if (ptr && handle.is_valid()) {
        submitted_mesh_map_[ptr] = handle;
    }
}

void RenderPipeline::submit(IMesh* mesh, const Material* material,
                            const math::Matrix4f& transform) {
    auto it = submitted_mesh_map_.find(mesh);
    if (it == submitted_mesh_map_.end() || !it->second.is_valid()) {
        GLOG_WARN("RenderPipeline::submit: mesh 未注册（请通过 Renderer 的 "
                  "load_mesh/create_mesh 创建）");
        return;
    }
    submit_items_.push_back(SubmitItem{it->second, material, transform});
}

void RenderPipeline::submit_instanced(IMesh* mesh, const Material* material,
                                      const math::Matrix4f* transforms, int count) {
    auto it = submitted_mesh_map_.find(mesh);
    if (it == submitted_mesh_map_.end() || !it->second.is_valid() || !transforms || count <= 0) {
        if (it == submitted_mesh_map_.end() || !it->second.is_valid()) {
            GLOG_WARN("RenderPipeline::submit_instanced: mesh 未注册（请通过 Renderer 的 "
                      "load_mesh/create_mesh 创建）");
        }
        return;
    }
    SubmitInstanceGroup group;
    group.mesh = it->second;
    group.material = material;
    group.transforms.assign(transforms, transforms + count);
    submit_instances_.push_back(std::move(group));
}

void RenderPipeline::set_submit_camera(const math::Vector3f& pos,
                                       const math::Matrix4f& view_proj) {
    // 从组合矩阵 P*V 反解出 math::Camera（GL 风格 z∈[-1,1] 的透视投影约定）。
    // 若调用方传入的不是标准透视 view_proj，则退化为只设置位置。
    const float* m = view_proj.m;
    const float row0_norm = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
    const float row1_norm = std::sqrt(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
    if (row0_norm < 1e-6f || row1_norm < 1e-6f || std::abs(m[12]) < 1e-6f) {
        submit_camera_.set_position(pos);
        camera_ = &submit_camera_;
        return;
    }

    const float f = row1_norm;
    const float aspect = f / row0_norm;
    const float A = -m[8] / m[12];
    const float B = m[10] + A * m[14];
    float near_plane = B / (A - 1.0f);
    float far_plane = B / (A + 1.0f);
    if (!(near_plane > 0.0f) || !(far_plane > near_plane)) {
        near_plane = 0.1f;
        far_plane = 100.0f;
    }
    const float fov = 2.0f * std::atan(1.0f / f);

    math::Vector3f forward(-m[2] * aspect / f, -m[6] / f, m[14]);
    if (forward.length_sq() < 1e-8f) {
        forward = math::Vector3f(0.0f, 0.0f, -1.0f);
    } else {
        forward = forward.normalized();
    }
    const float pitch = std::asin(math::clamp(forward.y, -1.0f, 1.0f));
    const float yaw = std::atan2(forward.z, forward.x);

    submit_camera_.set_position(pos);
    submit_camera_.set_fov(math::to_degrees(fov));
    submit_camera_.set_aspect(aspect);
    submit_camera_.set_near_far(near_plane, far_plane);
    submit_camera_.set_yaw(math::to_degrees(yaw));
    submit_camera_.set_pitch(math::to_degrees(pitch));
    camera_ = &submit_camera_;
}

void RenderPipeline::set_submit_lights(const LightData* lights, int count) {
    // 与 set_lights 相同的多光源管理：最多 k_max_lights 个，第一个方向光
    // （阴影投射）保留，其余按亮度优先级排序截断。
    lights_.clear();
    if (!lights || count <= 0) {
        shadow_light_index_ = -1;
        return;
    }
    std::vector<Light> converted;
    converted.reserve(static_cast<size_t>(count));
    const int n = std::min(count, static_cast<int>(k_max_lights));
    for (int i = 0; i < n; ++i) {
        Light l;
        l.type = static_cast<LightType>(lights[i].type);
        l.position = lights[i].position;
        l.direction = lights[i].direction;
        l.color = lights[i].color;
        l.intensity = lights[i].intensity;
        l.range = lights[i].range;
        l.spot_angle = lights[i].spot_angle;
        l.spot_softness = lights[i].spot_softness;
        converted.push_back(l);
    }

    lights_.reserve(converted.size());
    std::vector<Light> rest;
    rest.reserve(converted.size());
    bool has_shadow_light = false;
    for (const auto& l : converted) {
        if (!has_shadow_light && l.type == LightType::Directional) {
            lights_.push_back(l);
            has_shadow_light = true;
        } else {
            rest.push_back(l);
        }
    }
    const math::Vector3f cam_pos = camera_ ? camera_->position() : math::Vector3f::zero();
    auto priority = [&](const Light& l) {
        if (l.type == LightType::Directional) return l.intensity;
        return l.intensity / (1.0f + (l.position - cam_pos).length_sq());
    };
    std::sort(rest.begin(), rest.end(),
              [&](const Light& a, const Light& b) { return priority(a) > priority(b); });
    for (const auto& l : rest) {
        if (lights_.size() >= static_cast<size_t>(k_max_lights)) break;
        lights_.push_back(l);
    }
}

void RenderPipeline::set_submit_ambient(const math::Vector3f& color) {
    ambient_ = color;
}

void RenderPipeline::render_submitted_shadows(RenderContext& ctx) {
    // 与 render_scene 的传统级联阴影路径一致。Shadow Atlas / 点光源阴影
    // 需要 Scene 几何，submit 路径统一渲染到传统级联 FBO。
    for (int c = 0; c < cascade_count_; ++c) {
        const Frustum shadow_frustum = extract_frustum(cascade_light_space_matrices_[c]);
        begin_shadow_pass(ctx, c);

        RHIShaderHandle current_shadow = shadow_shader_;
        if (shadow_mode_ == ShadowMode::VSM) {
            current_shadow = shadow_vsm_shader_;
        } else if (shadow_mode_ == ShadowMode::ESM) {
            current_shadow = shadow_esm_shader_;
        }

        auto draw_submit_item = [&](const RHIMeshHandle& mesh, const Material* mat,
                                    const math::Matrix4f& model) {
            if (!mesh.is_valid()) return;
            ctx.set_uniform_mat4(current_shadow, "uModel", model);
            ctx.set_uniform_mat4(current_shadow, "uLightSpaceMatrix",
                                 cascade_light_space_matrices_[c]);
            if (mat && mat->albedo_texture().is_valid()) {
                ctx.set_texture(current_shadow, mat->albedo_texture(),
                                TextureSlots::kPBRAlbedo, "");
                ctx.set_uniform_int(current_shadow, "uAlbedoMap", TextureSlots::kPBRAlbedo);
                ctx.set_uniform_int(current_shadow, "uUseAlbedoMap", 1);
            } else {
                ctx.set_uniform_int(current_shadow, "uUseAlbedoMap", 0);
            }
            if (shadow_mode_ == ShadowMode::ESM) {
                ctx.set_uniform_float(current_shadow, "uESMExponent", esm_exponent_);
            }
            ctx.draw_mesh(mesh, current_shadow);
        };

        for (const auto& item : submit_items_) {
            draw_submit_item(item.mesh, item.material, item.model);
        }
        for (const auto& group : submit_instances_) {
            for (const auto& t : group.transforms) {
                draw_submit_item(group.mesh, group.material, t);
            }
        }
        end_shadow_pass(ctx);
    }
    if (shadow_mode_ == ShadowMode::VSM) {
        render_vsm_blur(ctx);
    }
}

void RenderPipeline::render_submitted(RenderContext& ctx) {
    if (!initialized_ || !camera_ || !has_submitted_items()) return;

    // 与 render_scene 相同的前缀
    if (sdfgi_enabled_ && sdfgi_.valid()) {
        std::vector<math::Vector3f> light_dirs;
        std::vector<math::Vector3f> light_colors;
        for (const auto& light : lights_) {
            if (light.type == LightType::Directional) {
                light_dirs.push_back(light.direction.normalized());
                light_colors.push_back(light.color * light.intensity);
            }
        }
        sdfgi_.update(camera_->position(), light_dirs, light_colors);
    }
    if (pp_params_.taa_enabled != 0) {
        ++taa_frame_;
    }
    last_bound_material_pbr_ = nullptr;
    last_bound_material_skinned_ = nullptr;

    update_light_space_matrix();
    const bool render_shadow = shadow_enabled_ && shadow_light_index_ >= 0;

    if (render_shadow) {
        render_submitted_shadows(ctx);
    }

    // 2. Forward PBR pass（submit 路径不走 deferred——GBuffer 需要 Scene 收集）
    if (deferred_enabled_ && hdr_enabled_) {
        GLOG_WARN("RenderPipeline::render_submitted: deferred 路径需要 Scene，"
                  "submit 路径使用 forward 渲染");
    }
    if (hdr_enabled_) {
        begin_hdr_forward_pass(ctx);
    } else {
        begin_forward_pass(ctx);
    }

    render_skybox(ctx);
    render_grid(ctx);
    bind_global_uniforms(ctx);

    // 收集 submit 队列 → 不透明/透明列表（透明按相机距离从远到近排序）
    auto& opaque_items = opaque_items_;
    auto& transparent_items = transparent_items_;
    opaque_items.clear();
    transparent_items.clear();
    const math::Vector3f cam_pos = camera_->position();
    auto push_item = [&](const RHIMeshHandle& mesh, const Material* mat,
                         const math::Matrix4f& model) {
        if (!mesh.is_valid()) return;
        const math::Vector3f pos(model(0, 3), model(1, 3), model(2, 3));
        const float dist_sq = (pos - cam_pos).length_sq();
        DrawItem item{mesh, mat, model, dist_sq};
        if (mat && mat->blend_mode == Material::BlendMode::Blend) {
            transparent_items.push_back(item);
        } else {
            opaque_items.push_back(item);
        }
    };
    for (const auto& item : submit_items_) {
        push_item(item.mesh, item.material, item.model);
    }
    for (const auto& group : submit_instances_) {
        for (const auto& t : group.transforms) {
            push_item(group.mesh, group.material, t);
        }
    }

    // 不透明物体
    ctx.set_blend(false);
    ctx.set_depth_write(true);
    for (const auto& item : opaque_items) {
        render_mesh_internal(item.mesh, item.material, item.model, ctx);
    }

    // 透明物体（远→近）
    if (!transparent_items.empty()) {
        std::sort(transparent_items.begin(), transparent_items.end(),
                  [](const DrawItem& a, const DrawItem& b) { return a.dist_sq > b.dist_sq; });
        ctx.set_blend(true);
        ctx.set_depth_write(false);
        for (const auto& item : transparent_items) {
            render_mesh_internal(item.mesh, item.material, item.model, ctx);
        }
        ctx.set_blend(false);
        ctx.set_depth_write(true);
    }

    // 清空队列
    submit_items_.clear();
    submit_instances_.clear();

    if (hdr_enabled_) {
        end_hdr_forward_pass(ctx);

        // 后处理（与 render_scene 相同；水面反射/贴花需要 Scene，submit 路径跳过）
        if (ssr_enabled_ && ssr_.valid()) {
            ssr_.render(&ctx, hdr_color_, hdr_depth_, gbuffer_normal_roughness_,
                        pp_params_, viewport_width_, viewport_height_);
        }
        if (ssil_enabled_ && ssil_.valid()) {
            ssil_.render(&ctx, hdr_color_, hdr_depth_, RHITextureHandle{},
                         pp_params_, viewport_width_, viewport_height_);
        }
        if (fog_enabled_ && fog_.valid() && camera_) {
            math::Matrix4f inv_vp = (get_projection_matrix() * camera_->get_view_matrix()).inverse();
            fog_.render(&ctx, hdr_depth_, inv_vp, camera_->get_view_matrix(),
                        camera_->position(), fog_color_, fog_density_, fog_height_,
                        camera_->near_plane(), camera_->far_plane());
            fog_.render_apply(&ctx, hdr_color_, hdr_depth_, inv_vp, camera_->position());
        }
        render_ssao(ctx);
        render_contact_shadow(ctx);
        render_bloom(ctx);
        if (dof_enabled_ && dof_.valid()) {
            dof_.render(&ctx, hdr_color_, hdr_depth_, pp_params_, viewport_width_, viewport_height_);
            if (dof_.dof_tex().is_valid()) {
                hdr_color_ = dof_.dof_tex();
            }
        }
        if (motion_blur_enabled_ && motion_blur_.valid()) {
            motion_blur_.render(&ctx, hdr_color_, hdr_depth_, RHITextureHandle{},
                                pp_params_, viewport_width_, viewport_height_);
            if (motion_blur_.output_tex().is_valid()) {
                hdr_color_ = motion_blur_.output_tex();
            }
        }
        render_auto_exposure(ctx);
        render_taa(ctx);
        render_tonemap(ctx);
    } else {
        end_forward_pass(ctx);
    }
}

void RenderPipeline::render_mesh_to_gbuffer(RHIMeshHandle mesh, const Material* material,
                                             const math::Matrix4f& model, RenderContext& ctx) {
    if (!mesh.is_valid() || !gbuffer_shader_.is_valid() || !camera_) return;

    const bool two_sided = material && material->two_sided;
    ctx.set_cull_face((cull_disabled_ || two_sided) ? CullMode::None : CullMode::Back);

    ctx.set_shader(gbuffer_shader_);
    ctx.set_uniform_mat4(gbuffer_shader_, "uModel", model);
    ctx.set_uniform_mat4(gbuffer_shader_, "uView", camera_->get_view_matrix());
    ctx.set_uniform_mat4(gbuffer_shader_, "uProjection", get_projection_matrix());
    ctx.set_uniform_int(gbuffer_shader_, "uTwoSided", two_sided ? 1 : 0);

    // Bind material properties to GBuffer shader
    if (material) {
        ctx.set_uniform_vec3(gbuffer_shader_, "uAlbedoColor", material->albedo_color);
        ctx.set_uniform_float(gbuffer_shader_, "uRoughness", material->roughness);
        ctx.set_uniform_float(gbuffer_shader_, "uMetallic", material->metallic);
        ctx.set_uniform_float(gbuffer_shader_, "uAO", material->ao);
        ctx.set_uniform_vec3(gbuffer_shader_, "uEmissiveColor", material->emissive_color);
        ctx.set_uniform_vec4(gbuffer_shader_, "uUVTransform",
                             math::Vector4f(material->uv_scale.x, material->uv_scale.y,
                                            material->uv_offset.x, material->uv_offset.y));
        ctx.set_uniform_float(gbuffer_shader_, "uOpacity", material->opacity);

        // Bind textures
        auto bind_gbuffer_tex = [&](const RHITextureHandle& tex_handle, int slot, const char* uniform_name) {
            if (tex_handle.is_valid()) {
                ITexture* tex = ctx_->texture(tex_handle);
                if (tex) tex->bind(slot);
                ctx.set_texture(gbuffer_shader_, tex_handle, slot, "");
                ctx.set_uniform_int(gbuffer_shader_, uniform_name, slot);
            }
        };

        bind_gbuffer_tex(material->albedo_texture(), TextureSlots::kPBRAlbedo, "uAlbedoMap");
        bind_gbuffer_tex(material->normal_texture(), TextureSlots::kPBRNormal, "uNormalMap");
        bind_gbuffer_tex(material->roughness_texture(), TextureSlots::kPBRRoughness, "uRoughnessMap");
        bind_gbuffer_tex(material->metallic_texture(), TextureSlots::kPBRMetallic, "uMetallicMap");
        bind_gbuffer_tex(material->ao_texture(), TextureSlots::kPBRAO, "uAOMap");
        bind_gbuffer_tex(material->emissive_texture(), TextureSlots::kPBREmissive, "uEmissiveMap");

        ctx.set_uniform_int(gbuffer_shader_, "uUseAlbedoMap", material->albedo_texture().is_valid() ? 1 : 0);
        ctx.set_uniform_int(gbuffer_shader_, "uUseNormalMap", material->normal_texture().is_valid() ? 1 : 0);
        ctx.set_uniform_int(gbuffer_shader_, "uUseRoughnessMap", material->roughness_texture().is_valid() ? 1 : 0);
        ctx.set_uniform_int(gbuffer_shader_, "uUseMetallicMap", material->metallic_texture().is_valid() ? 1 : 0);
        ctx.set_uniform_int(gbuffer_shader_, "uUseAOMap", material->ao_texture().is_valid() ? 1 : 0);
        ctx.set_uniform_int(gbuffer_shader_, "uUseEmissiveMap", material->emissive_texture().is_valid() ? 1 : 0);
    } else {
        // Default material
        ctx.set_uniform_vec3(gbuffer_shader_, "uAlbedoColor", math::Vector3f(1.0f, 1.0f, 1.0f));
        ctx.set_uniform_float(gbuffer_shader_, "uRoughness", 0.5f);
        ctx.set_uniform_float(gbuffer_shader_, "uMetallic", 0.0f);
        ctx.set_uniform_float(gbuffer_shader_, "uAO", 1.0f);
        ctx.set_uniform_vec3(gbuffer_shader_, "uEmissiveColor", math::Vector3f::zero());
        ctx.set_uniform_float(gbuffer_shader_, "uOpacity", 1.0f);
        ctx.set_uniform_int(gbuffer_shader_, "uUseAlbedoMap", 0);
        ctx.set_uniform_int(gbuffer_shader_, "uUseNormalMap", 0);
        ctx.set_uniform_int(gbuffer_shader_, "uUseRoughnessMap", 0);
        ctx.set_uniform_int(gbuffer_shader_, "uUseMetallicMap", 0);
        ctx.set_uniform_int(gbuffer_shader_, "uUseAOMap", 0);
        ctx.set_uniform_int(gbuffer_shader_, "uUseEmissiveMap", 0);
    }

    ctx.draw_mesh(mesh, gbuffer_shader_);
}

void RenderPipeline::render_skinned_mesh_to_gbuffer(RHIMeshHandle mesh, const Material* material,
                                                     const math::Matrix4f& model,
                                                     std::shared_ptr<const std::vector<math::Matrix4f>> palette,
                                                     RenderContext& ctx) {
    if (!mesh.is_valid() || !gbuffer_shader_.is_valid() || !camera_) return;

    const bool two_sided = material && material->two_sided;
    ctx.set_cull_face((cull_disabled_ || two_sided) ? CullMode::None : CullMode::Back);

    ctx.set_shader(gbuffer_shader_);
    ctx.set_uniform_mat4(gbuffer_shader_, "uModel", model);
    ctx.set_uniform_mat4(gbuffer_shader_, "uView", camera_->get_view_matrix());
    ctx.set_uniform_mat4(gbuffer_shader_, "uProjection", get_projection_matrix());
    ctx.set_uniform_int(gbuffer_shader_, "uTwoSided", two_sided ? 1 : 0);

    // Palette
    if (palette && !palette->empty()) {
        ctx.set_uniform_mat4_array(gbuffer_shader_, "uBonePalette", std::move(palette));
    }

    // Bind material (same as above)
    if (material) {
        ctx.set_uniform_vec3(gbuffer_shader_, "uAlbedoColor", material->albedo_color);
        ctx.set_uniform_float(gbuffer_shader_, "uRoughness", material->roughness);
        ctx.set_uniform_float(gbuffer_shader_, "uMetallic", material->metallic);
        ctx.set_uniform_float(gbuffer_shader_, "uAO", material->ao);
        ctx.set_uniform_vec3(gbuffer_shader_, "uEmissiveColor", material->emissive_color);
        ctx.set_uniform_vec4(gbuffer_shader_, "uUVTransform",
                             math::Vector4f(material->uv_scale.x, material->uv_scale.y,
                                            material->uv_offset.x, material->uv_offset.y));
        ctx.set_uniform_float(gbuffer_shader_, "uOpacity", material->opacity);

        auto bind_gbuffer_tex = [&](const RHITextureHandle& tex_handle, int slot, const char* uniform_name) {
            if (tex_handle.is_valid()) {
                ITexture* tex = ctx_->texture(tex_handle);
                if (tex) tex->bind(slot);
                ctx.set_texture(gbuffer_shader_, tex_handle, slot, "");
                ctx.set_uniform_int(gbuffer_shader_, uniform_name, slot);
            }
        };

        bind_gbuffer_tex(material->albedo_texture(), TextureSlots::kPBRAlbedo, "uAlbedoMap");
        bind_gbuffer_tex(material->normal_texture(), TextureSlots::kPBRNormal, "uNormalMap");
        bind_gbuffer_tex(material->roughness_texture(), TextureSlots::kPBRRoughness, "uRoughnessMap");
        bind_gbuffer_tex(material->metallic_texture(), TextureSlots::kPBRMetallic, "uMetallicMap");
        bind_gbuffer_tex(material->ao_texture(), TextureSlots::kPBRAO, "uAOMap");
        bind_gbuffer_tex(material->emissive_texture(), TextureSlots::kPBREmissive, "uEmissiveMap");

        ctx.set_uniform_int(gbuffer_shader_, "uUseAlbedoMap", material->albedo_texture().is_valid() ? 1 : 0);
        ctx.set_uniform_int(gbuffer_shader_, "uUseNormalMap", material->normal_texture().is_valid() ? 1 : 0);
        ctx.set_uniform_int(gbuffer_shader_, "uUseRoughnessMap", material->roughness_texture().is_valid() ? 1 : 0);
        ctx.set_uniform_int(gbuffer_shader_, "uUseMetallicMap", material->metallic_texture().is_valid() ? 1 : 0);
        ctx.set_uniform_int(gbuffer_shader_, "uUseAOMap", material->ao_texture().is_valid() ? 1 : 0);
        ctx.set_uniform_int(gbuffer_shader_, "uUseEmissiveMap", material->emissive_texture().is_valid() ? 1 : 0);
    } else {
        ctx.set_uniform_vec3(gbuffer_shader_, "uAlbedoColor", math::Vector3f(1.0f, 1.0f, 1.0f));
        ctx.set_uniform_float(gbuffer_shader_, "uRoughness", 0.5f);
        ctx.set_uniform_float(gbuffer_shader_, "uMetallic", 0.0f);
        ctx.set_uniform_float(gbuffer_shader_, "uAO", 1.0f);
        ctx.set_uniform_vec3(gbuffer_shader_, "uEmissiveColor", math::Vector3f::zero());
        ctx.set_uniform_float(gbuffer_shader_, "uOpacity", 1.0f);
        ctx.set_uniform_int(gbuffer_shader_, "uUseAlbedoMap", 0);
        ctx.set_uniform_int(gbuffer_shader_, "uUseNormalMap", 0);
        ctx.set_uniform_int(gbuffer_shader_, "uUseRoughnessMap", 0);
        ctx.set_uniform_int(gbuffer_shader_, "uUseMetallicMap", 0);
        ctx.set_uniform_int(gbuffer_shader_, "uUseAOMap", 0);
        ctx.set_uniform_int(gbuffer_shader_, "uUseEmissiveMap", 0);
    }

    ctx.draw_mesh(mesh, gbuffer_shader_);
}

void RenderPipeline::begin_forward_pass(RenderContext& ctx) {
    ctx.set_shader(pbr_shader_);
    ctx.set_framebuffer(RHIFramebufferHandle{});
    ctx.set_viewport(0, 0, viewport_width_, viewport_height_);
    ctx.set_depth_test(true);
    ctx.set_depth_write(true);
    ctx.set_cull_face(cull_disabled_ ? CullMode::None : CullMode::Back);
}

void RenderPipeline::end_forward_pass(RenderContext& ctx) {
    (void)ctx;
}

math::Matrix4f RenderPipeline::get_projection_matrix() const {
    if (!camera_) return math::Matrix4f::identity();
    math::Matrix4f proj = camera_->get_projection_matrix();
    // TAA 半像素抖动：Halton 序列逐帧偏移投影矩阵平移项
    if (pp_params_.taa_enabled != 0 && viewport_width_ > 0 && viewport_height_ > 0) {
        const float hx = (halton(taa_frame_, 2) - 0.5f) * 2.0f /
                         static_cast<float>(viewport_width_);
        const float hy = (halton(taa_frame_, 3) - 0.5f) * 2.0f /
                         static_cast<float>(viewport_height_);
        proj(0, 3) += hx;
        proj(1, 3) += hy;
    }
    if (ctx_ && ctx_->backend() && std::strcmp(ctx_->backend()->api_name(), "Vulkan") == 0) {
        // OpenGL NDC z ∈ [-1,1]，Vulkan z ∈ [0,1]。调整投影矩阵的 z 行。
        const float near_plane = camera_->near_plane();
        const float far_plane = camera_->far_plane();
        proj(2, 2) = far_plane / (near_plane - far_plane);
        proj(2, 3) = (far_plane * near_plane) / (near_plane - far_plane);
    }
    return proj;
}

void RenderPipeline::bind_per_frame_uniforms(RenderContext& ctx, RHIShaderHandle shader) {
    if (!shader.is_valid() || !camera_) return;
    // uniform 命令作用于"当前绑定"的 program（见 GLShader::set_*），
    // 上传前必须先绑定目标 shader。
    ctx.set_shader(shader);
    ctx.set_uniform_mat4(shader, "uView", camera_->get_view_matrix());
    ctx.set_uniform_mat4(shader, "uProjection", get_projection_matrix());
    // GL 的 PBR 顶点已不再使用 uLightSpaceMatrix（级联矩阵由片段阶段从 UBO 读取）；
    // Vulkan 仍需经 set_mat4 同步 push constant 状态。
    if (ctx.backend() && std::strcmp(ctx.backend()->api_name(), "Vulkan") == 0) {
        ctx.set_uniform_mat4(shader, "uLightSpaceMatrix", light_space_matrix_);
    }
    ctx.set_uniform_vec3(shader, "uCameraPos", camera_->position());
    ctx.set_uniform_vec3(shader, "uAmbient", ambient_);
    ctx.set_uniform_int(shader, "uHDREnabled", hdr_enabled_ ? 1 : 0);
    upload_lights(ctx, shader);
    upload_ibl_textures(ctx, shader);

    const bool use_shadow = shadow_enabled_ && shadow_light_index_ >= 0 && shadow_maps_[0].is_valid();

    // Shadow Atlas 模式：绑定图集纹理和 slot 变换信息
    if (shadow_atlas_enabled_ && shadow_atlas_.valid()) {
        ITexture* atlas_tex_ptr = ctx_->texture(shadow_atlas_.atlas_tex());
        if (atlas_tex_ptr) {
            atlas_tex_ptr->bind(TextureSlots::kPBRShadow);
        }
        ctx.set_texture(shader, shadow_atlas_.atlas_tex(), TextureSlots::kPBRShadow, "");
        ctx.set_uniform_int(shader, "uShadowMap", TextureSlots::kPBRShadow);
        ctx.set_uniform_int(shader, "uShadowAtlasEnabled", 1);
        // 传递 atlas 尺寸（用于 shader 中计算 slot UV）
        ctx.set_uniform_float(shader, "uShadowAtlasSize", (float)shadow_atlas_.atlas_size());
    } else {
        ctx.set_uniform_int(shader, "uShadowAtlasEnabled", 0);
        // 级联 0（兼容旧路径/旧 SPIR-V）
        if (shadow_maps_[0].is_valid()) {
            ITexture* shadow_map_ptr = ctx_->texture(shadow_maps_[0]);
            if (shadow_map_ptr) {
                shadow_map_ptr->bind(TextureSlots::kPBRShadow);
            }
            ctx.set_texture(shader, shadow_maps_[0], TextureSlots::kPBRShadow, "");
            ctx.set_uniform_int(shader, "uShadowMap", TextureSlots::kPBRShadow);
        }
    }
    // 级联 1..3 比较 sampler + PCSS 原始深度 sampler
    static constexpr int shadow_slots[k_max_cascades] = {
        TextureSlots::kPBRShadow, TextureSlots::kPBRShadowC1,
        TextureSlots::kPBRShadowC2, TextureSlots::kPBRShadowC3,
    };
    static constexpr int depth_slots[k_max_cascades] = {
        TextureSlots::kPBRShadowDepth, TextureSlots::kPBRShadowDepth1,
        TextureSlots::kPBRShadowDepth2, TextureSlots::kPBRShadowDepth3,
    };
    static constexpr const char* shadow_names[k_max_cascades] = {
        "uShadowMap", "uShadowMap1", "uShadowMap2", "uShadowMap3",
    };
    static constexpr const char* depth_names[k_max_cascades] = {
        "uShadowMapDepth", "uShadowMapDepth1", "uShadowMapDepth2", "uShadowMapDepth3",
    };
    // 固定绑定全部 4 级 shadow sampler（即使 cascade_count_ 较小），
    // 避免未绑定的级联 binding 落到 1x1 回退贴图（RGBA8 配深度比较 sampler 会触发
    // VUID-vkCmdDrawIndexed-None-06479），也保证 hot_reload 后首帧立即可用。
    for (int i = 1; i < k_max_cascades; ++i) {
        if (!shadow_maps_[i].is_valid()) continue;
        ITexture* tex = ctx_->texture(shadow_maps_[i]);
        if (tex) tex->bind(shadow_slots[i]);
        ctx.set_texture(shader, shadow_maps_[i], shadow_slots[i], "");
        ctx.set_uniform_int(shader, shadow_names[i], shadow_slots[i]);
    }
    for (int i = 0; i < k_max_cascades; ++i) {
        if (!shadow_maps_[i].is_valid()) continue;
        // 原始深度（非比较）采样：PCSS blocker search 需要真实深度值。
        // 经命令队列在渲染线程绑定（OpenGL 绑非比较 sampler）。
        ctx.set_texture_raw_depth(shader, shadow_maps_[i], depth_slots[i], "");
        ctx.set_uniform_int(shader, depth_names[i], depth_slots[i]);
    }
    ctx.set_uniform_int(shader, "uUseShadowMap", use_shadow ? 1 : 0);

    // ---- CSM 级联参数 ----
    ctx.set_uniform_int(shader, "uCascadeCount", cascade_count_);
    ctx.set_uniform_int(shader, "uPCSSEnabled", pcss_enabled_ ? 1 : 0);
    ctx.set_uniform_int(shader, "uDebugMode", debug_view_);

    // 分割边界：xyz = near, s1, s2, s3（w 未用）
    math::Vector4f splits(cascade_split_distances_[0],
                          cascade_split_distances_[1],
                          cascade_split_distances_[2],
                          cascade_split_distances_[3]);
    ctx.set_uniform_vec4(shader, "uCascadeSplits", splits);
    // x = far（最后一级远平面），y = 级联混合带宽比例，z/w 未用
    math::Vector4f far_blend(cascade_split_distances_[cascade_count_], 0.15f, 0.0f, 0.0f);
    ctx.set_uniform_vec4(shader, "uCascadeFarBlend", far_blend);

    // 每级 bias
    math::Vector4f biases(cascade_biases_[0], cascade_biases_[1],
                          cascade_biases_[2], cascade_biases_[3]);
    ctx.set_uniform_vec4(shader, "uCascadeBias", biases);

    // PCSS 参数
    ctx.set_uniform_float(shader, "uPCSSLightSize", pcss_light_size_);
    ctx.set_uniform_float(shader, "uPCSSMaxRadius", pcss_max_radius_);
    ctx.set_uniform_float(shader, "uPCSSBlockerScale", pcss_tap_scale_);

    // ---- VSM/ESM 阴影绑定 ----
    ctx.set_uniform_int(shader, "uShadowMode", static_cast<int>(shadow_mode_));
    ctx.set_uniform_float(shader, "uESMExponent", esm_exponent_);
    // VSM/ESM 纹理绑定到 slot 45-48（与 vsm_esm_shadow.h 中 k_vsm_tex_slot 一致）
    static constexpr int vsm_slots[k_max_cascades] = {45, 46, 47, 48};
    static constexpr const char* vsm_names[k_max_cascades] = {
        "uVSMTexture0", "uVSMTexture1", "uVSMTexture2", "uVSMTexture3",
    };
    for (int i = 0; i < k_max_cascades; ++i) {
        if (!vsm_color_tex_[i].is_valid()) continue;
        ITexture* tex = ctx_->texture(vsm_color_tex_[i]);
        if (tex) tex->bind(vsm_slots[i]);
        ctx.set_texture(shader, vsm_color_tex_[i], vsm_slots[i], "");
        ctx.set_uniform_int(shader, vsm_names[i], vsm_slots[i]);
    }

    // 每级 light view-proj（Vulkan 后端会做 NDC z 重映射）
    auto cascade_mats = std::make_shared<std::vector<math::Matrix4f>>();
    cascade_mats->reserve(k_max_cascades);
    for (int i = 0; i < k_max_cascades; ++i) {
        cascade_mats->push_back(cascade_light_space_matrices_[i]);
    }
    ctx.set_uniform_mat4_array(shader, "uCascadeLightSpace", std::move(cascade_mats));

    // 屏幕空间 AO（始终绑定合法纹理；禁用时绑 1x1 白纹理并以 uUseSSAO 跳过）
    const bool use_ssao = pp_params_.ssao_enabled != 0 && ssao_targets_valid_;
    const RHITextureHandle ssao_tex = use_ssao
                                          ? ssao_tex_[1]
                                          : (ssao_fallback_tex_.is_valid() ? ssao_fallback_tex_
                                                                            : ssao_tex_[0]);
    ITexture* ssao_ptr = ctx_->texture(ssao_tex);
    if (ssao_ptr) ssao_ptr->bind(TextureSlots::kPBRSSAO);
    ctx.set_texture(shader, ssao_tex, TextureSlots::kPBRSSAO, "");
    ctx.set_uniform_int(shader, "uSSAOTexture", TextureSlots::kPBRSSAO);
    ctx.set_uniform_int(shader, "uUseSSAO", use_ssao ? 1 : 0);
    ctx.set_uniform_float(shader, "uSSAOStrength", pp_params_.ssao_strength);

    // SSIL 屏幕空间间接光照
    const bool use_ssil = ssil_enabled_ && ssil_.valid() && ssil_.ssil_tex().is_valid();
    if (use_ssil) {
        ITexture* ssil_ptr = ctx_->texture(ssil_.ssil_tex());
        if (ssil_ptr) ssil_ptr->bind(TextureSlots::kSSILTexture);
        ctx.set_texture(shader, ssil_.ssil_tex(), TextureSlots::kSSILTexture, "");
    }
    ctx.set_uniform_int(shader, "uUseSSIL", use_ssil ? 1 : 0);

    // SDFGI 间接光照（已废弃，使用新版 GI 统一绑定）
    // 保留旧绑定以兼容旧 shader
    const bool use_sdfgi = sdfgi_enabled_ && sdfgi_.valid() && sdfgi_.gi_texture().is_valid();
    if (use_sdfgi) {
        ITexture* sdfgi_ptr = ctx_->texture(sdfgi_.gi_texture());
        if (sdfgi_ptr) sdfgi_ptr->bind(TextureSlots::kSDFGITexture);
        ctx.set_texture(shader, sdfgi_.gi_texture(), TextureSlots::kSDFGITexture, "");
    }
    ctx.set_uniform_int(shader, "uUseSDFGI", use_sdfgi ? 1 : 0);

    // ---- GI 统一绑定（GIMode: None/SDFGI/VoxelGI） ----
    const int gi_mode_val = static_cast<int>(gi_mode_);
    RHITextureHandle gi_tex;
    bool gi_active = gi_enabled_;
    if (gi_active && gi_mode_ == GIMode::SDFGI && sdfgi_.valid() && sdfgi_.gi_texture().is_valid()) {
        gi_tex = sdfgi_.gi_texture();
    } else if (gi_active && gi_mode_ == GIMode::VoxelGI && voxel_gi_.valid() && voxel_gi_.voxel_tex().is_valid()) {
        gi_tex = voxel_gi_.voxel_tex();
    } else {
        gi_active = false;
    }

    if (gi_active && gi_tex.is_valid()) {
        ITexture* gi_ptr = ctx_->texture(gi_tex);
        if (gi_ptr) gi_ptr->bind(TextureSlots::kGITexture);
        ctx.set_texture(shader, gi_tex, TextureSlots::kGITexture, "");
    }
    ctx.set_uniform_int(shader, "uGIEnabled", gi_active ? 1 : 0);
    ctx.set_uniform_int(shader, "uGIMode", gi_mode_val);
    ctx.set_uniform_float(shader, "uGIIndirectIntensity", gi_indirect_intensity_);
}

void RenderPipeline::bind_global_uniforms(RenderContext& ctx) {
    bind_per_frame_uniforms(ctx, pbr_shader_);
    bind_per_frame_uniforms(ctx, skinned_pbr_shader_);
    bind_point_shadow_uniforms(ctx, pbr_shader_);
    bind_point_shadow_uniforms(ctx, skinned_pbr_shader_);
}

void RenderPipeline::upload_lights(RenderContext& ctx, RHIShaderHandle shader) {
    if (!shader.is_valid()) return;
    const int count = static_cast<int>(std::min(lights_.size(), static_cast<size_t>(k_max_lights)));
    ctx.set_uniform_int(shader, "uLightCount", count);
    ctx.set_uniform_int(shader, "uShadowLightIndex", shadow_light_index_);

    char name[48];
    for (int i = 0; i < count; ++i) {
        const Light& light = lights_[i];

        std::snprintf(name, sizeof(name), "uLightType[%d]", i);
        ctx.set_uniform_int(shader, name, static_cast<int>(light.type));

        std::snprintf(name, sizeof(name), "uLightPos[%d]", i);
        ctx.set_uniform_vec3(shader, name, light.position);

        std::snprintf(name, sizeof(name), "uLightDir[%d]", i);
        ctx.set_uniform_vec3(shader, name, light.direction);

        std::snprintf(name, sizeof(name), "uLightColor[%d]", i);
        ctx.set_uniform_vec3(shader, name, light.color);

        // 物理光照单位：点/聚光 lumen → candela（÷4π），方向光按 lux 直传
        float intensity = light.intensity;
        if (physical_light_units_ && light.type != LightType::Directional) {
            intensity = light.intensity / (4.0f * 3.14159265f);
        }
        std::snprintf(name, sizeof(name), "uLightIntensity[%d]", i);
        ctx.set_uniform_float(shader, name, intensity);

        // x=range, y=cos(outer), z=cos(inner)
        const float outer = light.spot_angle * 3.14159265f / 180.0f;
        const float inner = light.spot_angle * (1.0f - light.spot_softness) * 3.14159265f / 180.0f;
        std::snprintf(name, sizeof(name), "uLightParams[%d]", i);
        ctx.set_uniform_vec4(shader, name,
                             math::Vector4f(light.range, std::cos(outer), std::cos(inner), 0.0f));
    }
}

void RenderPipeline::upload_ibl_textures(RenderContext& ctx, RHIShaderHandle shader) {
    if (!shader.is_valid()) return;
    const bool use_ibl = ibl_irradiance_texture_.is_valid() && ibl_prefilter_texture_.is_valid() &&
                         ibl_brdf_lut_texture_.is_valid();
    ctx.set_uniform_int(shader, "uUseIBL", use_ibl ? 1 : 0);
    ctx.set_uniform_float(shader, "uIBLIntensity", ibl_intensity_);

    if (!use_ibl) return;

    ITexture* irradiance = ctx_->texture(ibl_irradiance_texture_);
    if (irradiance) irradiance->bind(TextureSlots::kIBLIrradiance);
    ctx.set_texture(shader, ibl_irradiance_texture_, TextureSlots::kIBLIrradiance, "");
    ctx.set_uniform_int(shader, "uIrradianceMap", TextureSlots::kIBLIrradiance);

    ITexture* prefilter = ctx_->texture(ibl_prefilter_texture_);
    if (prefilter) prefilter->bind(TextureSlots::kIBLPrefilter);
    ctx.set_texture(shader, ibl_prefilter_texture_, TextureSlots::kIBLPrefilter, "");
    ctx.set_uniform_int(shader, "uPrefilterMap", TextureSlots::kIBLPrefilter);

    ITexture* brdf = ctx_->texture(ibl_brdf_lut_texture_);
    if (brdf) brdf->bind(TextureSlots::kIBLBRDF);
    ctx.set_texture(shader, ibl_brdf_lut_texture_, TextureSlots::kIBLBRDF, "");
    ctx.set_uniform_int(shader, "uBRDFLUT", TextureSlots::kIBLBRDF);
}

bool RenderPipeline::rebuild(RenderContext* ctx, const std::string& shader_dir) {
    if (!ctx) return false;
    GLOG_INFO("RenderPipeline: rebuilding...");
    shutdown();
    const bool ok = init(ctx, shader_dir);
    if (ok) {
        GLOG_INFO("RenderPipeline: rebuild succeeded");
    } else {
        GLOG_ERROR("RenderPipeline: rebuild failed");
    }
    return ok;
}

bool RenderPipeline::hot_reload() {
    if (!ctx_ || !initialized_) return false;

    RenderContext* ctx = ctx_;

    // 旧 viewport 输出纹理的 ImGui descriptor 缓存作废（重建后是新纹理句柄）
    if (imgui_backend_ && viewport_color_.is_valid()) {
        if (ITexture* old_tex = ctx->texture(viewport_color_)) {
            imgui_backend_->invalidate_texture(old_tex);
        }
    }

    // shutdown() 会销毁天空盒/环境/LUT 的 GPU 资源并清空标记/路径，
    // 必须先把需要恢复的配置复制到局部变量。
    const std::array<std::string, 6> saved_skybox_paths = skybox_paths_;
    const bool reapply_skybox = skybox_set_ && !saved_skybox_paths[0].empty();
    const std::string saved_env_hdr_path = environment_hdr_path_;
    const bool reapply_env_hdr = environment_set_ && !saved_env_hdr_path.empty();
    const bool reapply_env_from_skybox = environment_from_skybox_;
    const std::string saved_lut_path = lut_path_;
    const bool reapply_lut = lut_set_ && !saved_lut_path.empty();

    GLOG_INFO("RenderPipeline: hot reload started (backend={}, shader_dir='{}')",
              ctx->backend() ? ctx->backend()->api_name() : "?", shader_dir_);

    // 暂停渲染线程：等待当前帧完成、GPU idle、context 切回主线程，
    // 之后 shutdown()/init() 中的 GL/VK 调用都安全。调用方需在 present() 之后触发。
    const bool was_running = ctx->is_running();
    ctx->pause_render_thread();
    // 同步模式（编辑器）没有渲染线程，pause_render_thread() 是空操作，
    // 必须显式等待 GPU 完成所有 in-flight 命令，否则 shutdown() 销毁的
    // VkBuffer/Framebuffer/RenderPass 仍被上一帧提交的命令缓冲引用，
    // 触发 vkDestroy* "currently in use" 校验错误并造成内存损坏。
    if (ctx->backend()) {
        ctx->backend()->wait_gpu_idle();
    }

    shutdown();
    bool ok = init(ctx, shader_dir_);
    if (ok) {
        // 恢复外部配置（其余参数如 hdr/tonemap/shadow/cascade/pp_params 是成员，
        // shutdown() 不清除，自动保留）
        if (reapply_skybox && !set_skybox(saved_skybox_paths)) {
            GLOG_WARN("RenderPipeline: hot reload could not restore skybox");
        }
        if (reapply_env_hdr) {
            if (!set_environment_hdr(saved_env_hdr_path)) {
                GLOG_WARN("RenderPipeline: hot reload could not restore HDR environment");
            }
        } else if (reapply_env_from_skybox) {
            if (!set_environment_from_skybox()) {
                GLOG_WARN("RenderPipeline: hot reload could not restore skybox-derived environment");
            }
        }
        if (reapply_lut) {
            // 暂停期间 cmd_buffer 为空，运行时 push_command 路径是 no-op，
            // 这里直接同步重建 LUT 纹理。
            auto tex_data = assets::AssetManager::instance().load<assets::TextureData>(saved_lut_path);
            if (tex_data.valid() && tex_data.get()) {
                if (!create_lut_texture(ctx, tex_data.get())) {
                    GLOG_WARN("RenderPipeline: hot reload could not restore color LUT");
                }
            } else {
                GLOG_WARN("RenderPipeline: hot reload could not reload color LUT '{}'", saved_lut_path);
            }
        }
    }

    if (was_running) {
        ctx->resume_render_thread();
    }

    GLOG_INFO("RenderPipeline: hot reload {}", ok ? "succeeded" : "failed");
    return ok;
}

bool RenderPipeline::resize_render_targets(int width, int height) {
    if (width == viewport_width_ && height == viewport_height_) return true;

    viewport_width_ = width;
    viewport_height_ = height;

    // 先创建新目标再销毁旧目标：销毁命令经 pending 队列延迟执行，
    // 若先销毁再创建，句柄槽位可能被立即复用导致悬垂引用。
    if (hdr_enabled_) {
        RHITextureHandle old_color = hdr_color_;
        RHITextureHandle old_depth = hdr_depth_;
        RHIFramebufferHandle old_fbo = hdr_fbo_;
        hdr_color_ = RHITextureHandle{};
        hdr_depth_ = RHITextureHandle{};
        hdr_fbo_ = RHIFramebufferHandle{};

        if (!create_hdr_target(ctx_)) {
            GLOG_ERROR("RenderPipeline: resize HDR target failed ({}x{})", width, height);
            // 回滚：销毁可能创建了一半的新目标，恢复旧目标
            if (hdr_color_.is_valid()) ctx_->destroy_texture(hdr_color_);
            if (hdr_depth_.is_valid()) ctx_->destroy_texture(hdr_depth_);
            if (hdr_fbo_.is_valid()) ctx_->destroy_framebuffer(hdr_fbo_);
            hdr_color_ = old_color;
            hdr_depth_ = old_depth;
            hdr_fbo_ = old_fbo;
            return false;
        }
        if (old_color.is_valid()) ctx_->destroy_texture(old_color);
        if (old_depth.is_valid()) ctx_->destroy_texture(old_depth);
        if (old_fbo.is_valid()) ctx_->destroy_framebuffer(old_fbo);
    }

    if (viewport_output_enabled_) {
        RHITextureHandle old_color = viewport_color_;
        RHIFramebufferHandle old_fbo = viewport_fbo_;
        viewport_color_ = RHITextureHandle{};
        viewport_fbo_ = RHIFramebufferHandle{};

        // 先 invalidate 旧的 ImGui descriptor set 缓存，避免视口面板显示已销毁纹理
        if (imgui_backend_ && old_color.is_valid()) {
            ITexture* old_tex = ctx_->texture(old_color);
            if (old_tex) imgui_backend_->invalidate_texture(old_tex);
        }

        if (!create_viewport_target(ctx_)) {
            GLOG_ERROR("RenderPipeline: resize viewport target failed ({}x{})", width, height);
            if (viewport_color_.is_valid()) ctx_->destroy_texture(viewport_color_);
            if (viewport_fbo_.is_valid()) ctx_->destroy_framebuffer(viewport_fbo_);
            viewport_color_ = old_color;
            viewport_fbo_ = old_fbo;
            return false;
        }
        if (old_color.is_valid()) ctx_->destroy_texture(old_color);
        if (old_fbo.is_valid()) ctx_->destroy_framebuffer(old_fbo);
    }

    // Bloom 目标：调用方保证渲染线程已暂停（pending 销毁已排空），直接重建
    if (hdr_enabled_) {
        destroy_bloom_targets();
        if (!create_bloom_targets(ctx_)) {
            GLOG_ERROR("RenderPipeline: resize bloom targets failed, bloom disabled");
            pp_params_.bloom_enabled = 0;
        }
        destroy_auto_exposure_targets();
        if (!create_auto_exposure_targets(ctx_)) {
            GLOG_WARN("RenderPipeline: resize auto-exposure targets failed, disabled");
            pp_params_.auto_exposure = 0;
        }
        destroy_taa_targets();
        if (!create_taa_targets(ctx_)) {
            GLOG_WARN("RenderPipeline: resize TAA targets failed, disabled");
            pp_params_.taa_enabled = 0;
        }
        destroy_ssao_targets();
        if (!create_ssao_targets(ctx_)) {
            GLOG_WARN("RenderPipeline: resize GTAO targets failed, disabled");
            pp_params_.ssao_enabled = 0;
        }
        // GBuffer resize
        if (deferred_enabled_ && gbuffer_targets_valid_) {
            destroy_gbuffer_targets();
            if (!create_gbuffer_targets(ctx_)) {
                GLOG_WARN("RenderPipeline: resize GBuffer targets failed, deferred disabled");
                deferred_enabled_ = false;
            }
        }

        // SSR HiZ
        if (ssr_.valid()) {
            ssr_.create_hiz(width, height);
        }

        // 体积雾 target
        if (fog_.valid()) {
            fog_.create_targets(width, height);
        }

        // DOF target
        if (dof_.valid()) {
            dof_.create_targets(width, height);
        }

        // Motion Blur target
        if (motion_blur_.valid()) {
            motion_blur_.create_targets(width, height);
        }

        // SSIL target
        if (ssil_.valid()) {
            ssil_.create_targets(width, height);
        }

        // Water target
        if (water_.valid()) {
            water_.create_targets(width, height);
        }
    }
    return true;
}

bool RenderPipeline::create_fullscreen_mesh(RenderContext* ctx) {
    fullscreen_mesh_ = ctx->create_mesh();
    IMesh* mesh_ptr = ctx->mesh(fullscreen_mesh_);
    if (!fullscreen_mesh_.is_valid() || !mesh_ptr) return false;

    // 一个覆盖全屏的三角形，包含 position(uv) 和 texcoord
    struct Vertex {
        float x, y;
        float u, v;
    };
    Vertex verts[] = {
        {-1.0f, -1.0f, 0.0f, 0.0f},
        { 3.0f, -1.0f, 2.0f, 0.0f},
        {-1.0f,  3.0f, 0.0f, 2.0f}
    };

    mesh_ptr->upload_vertices(verts, sizeof(verts), 3);
    VertexLayout layout;
    layout.stride = sizeof(Vertex);
    layout.attributes = {
        {0, VertexType::Float2, false, 0},
        {1, VertexType::Float2, false, 2 * sizeof(float)}
    };
    mesh_ptr->set_layout(layout);
    return true;
}

void RenderPipeline::begin_hdr_forward_pass(RenderContext& ctx) {
    GLOG_DEBUG("RenderPipeline::begin_hdr_forward_pass: hdr_fbo_={} viewport={}x{}",
              hdr_fbo_.index, viewport_width_, viewport_height_);
    ctx.set_shader(pbr_shader_);
    ctx.set_framebuffer(hdr_fbo_);
    ctx.set_viewport(0, 0, viewport_width_, viewport_height_);
    // 深度清除受 glDepthMask 影响：上一帧 grid/透明 pass 会把深度写关闭，
    // 必须先恢复深度写，否则 glClear(DEPTH_BUFFER_BIT) 是空操作，深度缓冲
    // 保留旧值，场景物体全部被深度测试剔除（OpenGL 视口网格可见但物体不可见）。
    ctx.set_depth_test(true);
    ctx.set_depth_write(true);
    ctx.set_cull_face(cull_disabled_ ? CullMode::None : CullMode::Back);
    ctx.clear(0.15f, 0.15f, 0.18f, 1.0f);
    ctx.clear_depth();
}

void RenderPipeline::end_hdr_forward_pass(RenderContext& ctx) {
    (void)ctx;
}

bool RenderPipeline::create_grid_mesh(RenderContext* ctx) {
    // 大平面覆盖 XZ 平面，使用 MeshVertex 布局（position 有效，其余填 0）。
    struct VertexGPU {
        float x, y, z;
        float nx, ny, nz;
        float tx, ty, tz;
        float u, v;
        float r, g, b;
    };

    const float half = 500.0f;
    VertexGPU verts[] = {
        {-half, 0.0f, -half, 0, 1, 0, 1, 0, 0, 0, 0, 1, 1, 1},
        { half, 0.0f, -half, 0, 1, 0, 1, 0, 0, 1, 0, 1, 1, 1},
        { half, 0.0f,  half, 0, 1, 0, 1, 0, 0, 1, 1, 1, 1, 1},
        {-half, 0.0f,  half, 0, 1, 0, 1, 0, 0, 0, 1, 1, 1, 1},
    };
    const uint32_t indices[] = {0, 1, 2, 0, 2, 3};

    grid_mesh_ = ctx->create_mesh();
    IMesh* mesh_ptr = ctx->mesh(grid_mesh_);
    if (!grid_mesh_.is_valid() || !mesh_ptr) return false;
    mesh_ptr->upload_vertices(verts, sizeof(verts), 4);
    mesh_ptr->upload_indices(indices, sizeof(indices), 6);

    VertexLayout layout;
    layout.stride = sizeof(VertexGPU);
    layout.attributes = {
        {0, VertexType::Float3, false, 0},
        {1, VertexType::Float3, false, 3 * sizeof(float)},
        {2, VertexType::Float3, false, 6 * sizeof(float)},
        {3, VertexType::Float2, false, 9 * sizeof(float)},
        {4, VertexType::Float3, false, 11 * sizeof(float)}
    };
    mesh_ptr->set_layout(layout);
    return true;
}

void RenderPipeline::render_grid(RenderContext& ctx) {
    if (!grid_enabled_ || !grid_shader_.is_valid() || !grid_mesh_.is_valid() || !camera_) return;

    // 网格透明混合，深度测试开启但深度写入关闭，避免遮挡场景物体。
    ctx.set_depth_test(true);
    ctx.set_depth_write(false);
    ctx.set_cull_face(CullMode::None);
    ctx.set_blend(true);

    ctx.set_shader(grid_shader_);
    // 网格平面跟随相机在 XZ 平面上移动，避免相机远离原点后看不到网格。
    const math::Vector3f cam_pos = camera_->position();
    const math::Matrix4f grid_model = math::Matrix4f::translate(cam_pos.x, 0.0f, cam_pos.z);
    ctx.set_uniform_mat4(grid_shader_, "uModel", grid_model);
    ctx.set_uniform_mat4(grid_shader_, "uView", camera_->get_view_matrix());
    ctx.set_uniform_mat4(grid_shader_, "uProjection", get_projection_matrix());
    ctx.set_uniform_vec3(grid_shader_, "uGridColor", math::Vector3f(0.5f, 0.5f, 0.5f));
    ctx.set_uniform_float(grid_shader_, "uGridSize", k_grid_size);
    ctx.set_uniform_float(grid_shader_, "uMajorLineEvery", k_grid_major_every);
    ctx.set_uniform_float(grid_shader_, "uFadeStart", k_grid_fade_start);
    ctx.set_uniform_float(grid_shader_, "uFadeEnd", k_grid_fade_end);

    ctx.draw_mesh(grid_mesh_, grid_shader_);

    // 恢复默认状态（不透明物体需要这些状态）
    ctx.set_depth_write(true);
    ctx.set_cull_face(cull_disabled_ ? CullMode::None : CullMode::Back);
    ctx.set_blend(false);
}

// ===========================================================================
// Reflection Probe IBL 绑定
// ===========================================================================
void RenderPipeline::bind_probe_ibl(RenderContext& ctx, RHIShaderHandle shader, const math::Vector3f& position) {
    if (!shader.is_valid()) return;

    // 查找最近的 probe
    RHITextureHandle irr = probe_system_.nearest_irradiance(position);
    RHITextureHandle pre = probe_system_.nearest_prefilter(position);
    if (!irr.is_valid() || !pre.is_valid()) return;

    // 覆盖全局 IBL：绑定 probe 的 irradiance 和 prefilter 到标准 IBL slot
    // uIrradianceMap 和 uPrefilterMap 已在 upload_ibl_textures 中绑定，
    // 此处用 probe 的纹理覆盖，复用相同的 texture unit slot。
    ITexture* irr_tex = ctx_->texture(irr);
    if (irr_tex) {
        irr_tex->bind(TextureSlots::kIBLIrradiance);
        ctx.set_texture(shader, irr, TextureSlots::kIBLIrradiance, "");
    }

    ITexture* pre_tex = ctx_->texture(pre);
    if (pre_tex) {
        pre_tex->bind(TextureSlots::kIBLPrefilter);
        ctx.set_texture(shader, pre, TextureSlots::kIBLPrefilter, "");
    }
}

// ===========================================================================
// Decal 包围盒 mesh 创建（单位立方体）
// ===========================================================================
bool RenderPipeline::create_decal_box_mesh(RenderContext* ctx) {
    if (!ctx) return false;

    decal_box_mesh_ = ctx->create_mesh();
    IMesh* mesh = ctx->mesh(decal_box_mesh_);
    if (!decal_box_mesh_.is_valid() || !mesh) {
        GLOG_ERROR("RenderPipeline: failed to create decal box mesh");
        return false;
    }

    // 单位立方体顶点（只含位置，12 个三角形，每个三角形 3 个顶点）
    struct Vertex {
        float x, y, z;
    };
    static constexpr Vertex k_vertices[] = {
        // 正面 (+Z)
        {-0.5f, -0.5f,  0.5f}, { 0.5f, -0.5f,  0.5f}, { 0.5f,  0.5f,  0.5f},
        {-0.5f, -0.5f,  0.5f}, { 0.5f,  0.5f,  0.5f}, {-0.5f,  0.5f,  0.5f},
        // 背面 (-Z)
        {-0.5f,  0.5f, -0.5f}, { 0.5f,  0.5f, -0.5f}, { 0.5f, -0.5f, -0.5f},
        {-0.5f,  0.5f, -0.5f}, { 0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f, -0.5f},
        // 右面 (+X)
        { 0.5f, -0.5f, -0.5f}, { 0.5f,  0.5f, -0.5f}, { 0.5f,  0.5f,  0.5f},
        { 0.5f, -0.5f, -0.5f}, { 0.5f,  0.5f,  0.5f}, { 0.5f, -0.5f,  0.5f},
        // 左面 (-X)
        {-0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f,  0.5f}, {-0.5f,  0.5f,  0.5f},
        {-0.5f, -0.5f, -0.5f}, {-0.5f,  0.5f,  0.5f}, {-0.5f,  0.5f, -0.5f},
        // 上面 (+Y)
        {-0.5f,  0.5f, -0.5f}, {-0.5f,  0.5f,  0.5f}, { 0.5f,  0.5f,  0.5f},
        {-0.5f,  0.5f, -0.5f}, { 0.5f,  0.5f,  0.5f}, { 0.5f,  0.5f, -0.5f},
        // 下面 (-Y)
        {-0.5f, -0.5f, -0.5f}, { 0.5f, -0.5f, -0.5f}, { 0.5f, -0.5f,  0.5f},
        {-0.5f, -0.5f, -0.5f}, { 0.5f, -0.5f,  0.5f}, {-0.5f, -0.5f,  0.5f},
    };
    static constexpr int k_vertex_count = sizeof(k_vertices) / sizeof(Vertex);

    // 使用与 grid mesh 相同的 VertexLayout 模式
    struct DecalVertex {
        float x, y, z;
    };
    static_assert(sizeof(DecalVertex) == sizeof(Vertex));

    mesh->upload_vertices(k_vertices, k_vertex_count * sizeof(Vertex), k_vertex_count);

    VertexLayout layout;
    layout.stride = sizeof(DecalVertex);
    layout.attributes = {
        {0, VertexType::Float3, false, 0},
    };
    mesh->set_layout(layout);

    return true;
}

// ===========================================================================
// Decal 渲染（Forward 路径）
// ===========================================================================
void RenderPipeline::render_decal_forward(RenderContext& ctx) {
    if (!decal_shader_.is_valid() || !decal_box_mesh_.is_valid() || !camera_) return;
    if (decal_storage_.decal_count() == 0) return;

    const int decal_count = decal_storage_.decal_count();
    const auto& decals = decal_storage_.decals();

    // 绑定 depth 纹理（HDR 深度）
    RHITextureHandle depth_tex = hdr_enabled_ ? hdr_depth_ : RHITextureHandle{};
    if (!depth_tex.is_valid()) return;

    ITexture* depth_ptr = ctx_->texture(depth_tex);
    if (depth_ptr) depth_ptr->bind(kDecalDepthSlot);
    ctx.set_texture(decal_shader_, depth_tex, kDecalDepthSlot, "");
    ctx.set_uniform_int(decal_shader_, "uDepthTexture", kDecalDepthSlot);

    // 逆 VP 矩阵
    math::Matrix4f inv_vp = (get_projection_matrix() * camera_->get_view_matrix()).inverse();
    ctx.set_uniform_mat4(decal_shader_, "uInvViewProj", inv_vp);

    // 屏幕尺寸
    ctx.set_uniform_float(decal_shader_, "uScreenWidth", (float)viewport_width_);
    ctx.set_uniform_float(decal_shader_, "uScreenHeight", (float)viewport_height_);

    // 渲染状态：blend 开、深度读/写关（基于 depth buffer 投影）
    ctx.set_framebuffer(hdr_fbo_);
    ctx.set_viewport(0, 0, viewport_width_, viewport_height_);
    ctx.set_blend(true);
    ctx.set_depth_test(false);
    ctx.set_depth_write(false);
    ctx.set_cull_face(CullMode::None);

    ctx.set_shader(decal_shader_);
    ctx.set_uniform_mat4(decal_shader_, "uView", camera_->get_view_matrix());
    ctx.set_uniform_mat4(decal_shader_, "uProjection", get_projection_matrix());

    for (int i = 0; i < decal_count; ++i) {
        const DecalData& d = decals[i];
        if (!d.enabled) continue;

        // 构建 decal 世界矩阵（TRS: translate * rotate * scale）
        math::Matrix4f model = math::Matrix4f::translate(d.position);

        // 欧拉角旋转（ZYX 顺序）
        math::Matrix4f rot = math::Matrix4f::identity();
        if (std::abs(d.rotation.z) > 1e-6f)
            rot = rot * math::Matrix4f::rotate(d.rotation.z * 3.14159265f / 180.0f, math::Vector3f(0, 0, 1));
        if (std::abs(d.rotation.y) > 1e-6f)
            rot = rot * math::Matrix4f::rotate(d.rotation.y * 3.14159265f / 180.0f, math::Vector3f(0, 1, 0));
        if (std::abs(d.rotation.x) > 1e-6f)
            rot = rot * math::Matrix4f::rotate(d.rotation.x * 3.14159265f / 180.0f, math::Vector3f(1, 0, 0));
        model = model * rot;

        math::Matrix4f scale = math::Matrix4f::scale(d.scale);
        model = model * scale;

        ctx.set_uniform_mat4(decal_shader_, "uModel", model);

        // 世界 → 贴花局部矩阵（逆矩阵）
        math::Matrix4f world_to_decal = model.inverse();
        ctx.set_uniform_mat4(decal_shader_, "uWorldToDecal", world_to_decal);

        // 贴花属性
        ctx.set_uniform_vec3(decal_shader_, "uDecalAlbedo", d.albedo);
        ctx.set_uniform_float(decal_shader_, "uDecalOpacity", d.opacity);

        // 贴花纹理
        // 注意：DecalData 使用 float 存储纹理句柄（简化设计），
        // 实际使用时需要通过纹理系统传入合法的 RHITextureHandle。
        // 此处简化为仅使用颜色，纹理支持需要对接纹理管理系统。
        ctx.set_uniform_int(decal_shader_, "uUseAlbedoTex", 0);

        ctx.draw_mesh(decal_box_mesh_, decal_shader_);
    }

    // 恢复渲染状态
    ctx.set_blend(false);
    ctx.set_depth_test(true);
    ctx.set_depth_write(true);
}

// ===========================================================================
// Decal 渲染（Deferred 路径）
// ===========================================================================
void RenderPipeline::render_decal_deferred(RenderContext& ctx) {
    // Deferred 路径的 decal 渲染与 forward 路径相同：
    // 在 Lighting Pass 后，使用 box mesh 在 HDR 颜色上 blend decal 颜色
    render_decal_forward(ctx);
}

// ---------------------------------------------------------------------------
// 点光源双抛物面阴影
// ---------------------------------------------------------------------------
bool RenderPipeline::create_point_shadow_targets(RenderContext* ctx) {
    if (!ctx) return false;
    destroy_point_shadow_targets();

    const int count = static_cast<int>(k_max_point_shadows);
    point_shadow_tex_.resize(count);
    point_shadow_fbo_.resize(count);

    for (int i = 0; i < count; ++i) {
        // 创建 RGBA16F 纹理存储双抛物面深度
        point_shadow_tex_[i] = ctx->create_texture();
        ITexture* tex = ctx->texture(point_shadow_tex_[i]);
        if (!point_shadow_tex_[i].is_valid() || !tex ||
            !tex->create(TextureFormat::RGBA16F, point_shadow_size_, point_shadow_size_, nullptr)) {
            GLOG_ERROR("RenderPipeline: point shadow tex {} failed", i);
            destroy_point_shadow_targets();
            return false;
        }
        tex->set_filter(TextureFilter::Linear, TextureFilter::Linear);
        tex->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

        // 创建 FBO（无深度附件，深度写入颜色附件）
        point_shadow_fbo_[i] = ctx->create_framebuffer();
        IFramebuffer* fbo = ctx->framebuffer(point_shadow_fbo_[i]);
        if (!point_shadow_fbo_[i].is_valid() || !fbo ||
            !fbo->create(point_shadow_size_, point_shadow_size_)) {
            GLOG_ERROR("RenderPipeline: point shadow fbo {} failed", i);
            destroy_point_shadow_targets();
            return false;
        }
        fbo->attach_color_texture(tex);
        if (!fbo->is_complete()) {
            GLOG_ERROR("RenderPipeline: point shadow fbo {} incomplete", i);
            destroy_point_shadow_targets();
            return false;
        }
    }
    return true;
}

void RenderPipeline::destroy_point_shadow_targets() {
    if (!ctx_) return;
    for (auto& fb : point_shadow_fbo_) {
        if (fb.is_valid()) {
            ctx_->destroy_framebuffer(fb);
            fb = RHIFramebufferHandle{};
        }
    }
    point_shadow_fbo_.clear();
    for (auto& tex : point_shadow_tex_) {
        if (tex.is_valid()) {
            ctx_->destroy_texture(tex);
            tex = RHITextureHandle{};
        }
    }
    point_shadow_tex_.clear();
}

void RenderPipeline::render_point_shadows(RenderContext& ctx, scene::Scene& scene) {
    if (!point_shadow_enabled_ || !point_shadow_shader_.is_valid()) return;
    if (point_shadow_tex_.empty() || point_shadow_fbo_.empty()) return;

    // 统计场景中点光源数量
    int point_count = 0;
    for (const auto& light : lights_) {
        if (light.type == LightType::Point) ++point_count;
    }
    if (point_count == 0) return;

    const int shadow_count = std::min(point_count, static_cast<int>(k_max_point_shadows));

    int light_idx = 0;
    for (int s = 0; s < shadow_count; ++s) {
        // 找到下一个点光源
        while (light_idx < static_cast<int>(lights_.size()) &&
               lights_[light_idx].type != LightType::Point) {
            ++light_idx;
        }
        if (light_idx >= static_cast<int>(lights_.size())) break;

        const Light& light = lights_[light_idx];
        const math::Vector3f light_pos = light.position;
        const float range = light.range;

        // 双抛物面映射：分别渲染 front face (z>=0) 和 back face (z<0)
        for (int face = 0; face < 2; ++face) {
            ctx.set_shader(point_shadow_shader_);
            ctx.set_framebuffer(point_shadow_fbo_[s]);
            ctx.set_viewport(0, 0, point_shadow_size_, point_shadow_size_);
            // 不清除，直接覆盖（双抛物面两 face 共用一个纹理）
            if (face == 0) {
                ctx.clear(1.0f, 0.0f, 0.0f, 1.0f); // 默认深度 1.0（远）
            }
            ctx.set_depth_test(false); // 无深度缓冲，直接写入颜色
            ctx.set_depth_write(false);
            ctx.set_cull_face(CullMode::None);

            ctx.set_uniform_int(point_shadow_shader_, "uParaboloidFace", face);
            ctx.set_uniform_float(point_shadow_shader_, "uPointLightRange", range);
            ctx.set_uniform_vec3(point_shadow_shader_, "uPointLightPos", light_pos);

            // 遍历场景中所有 mesh
            ecs::foreach_with_components<components::MeshRenderer, components::Transform>(
                scene,
                [&](scene::Entity* entity, components::MeshRenderer* mr, components::Transform* /*transform*/) {
                    if (!mr->enabled || mr->mesh_path.empty() || !mr->gpu_mesh_handle().is_valid()) return;
                    const math::Matrix4f& model = entity->world_transform();

                    ctx.set_uniform_mat4(point_shadow_shader_, "uModel", model);

                    // 绑定 albedo 纹理做 alpha test（使用标准 PBR albedo slot）
                    if (mr->material && mr->material->albedo_texture().is_valid()) {
                        ctx.set_texture(point_shadow_shader_, mr->material->albedo_texture(),
                                        TextureSlots::kPBRAlbedo, "");
                        ctx.set_uniform_int(point_shadow_shader_, "uAlbedoMap", TextureSlots::kPBRAlbedo);
                        ctx.set_uniform_int(point_shadow_shader_, "uUseAlbedoMap", 1);
                    } else {
                        ctx.set_uniform_int(point_shadow_shader_, "uUseAlbedoMap", 0);
                    }
                    ctx.set_uniform_float(point_shadow_shader_, "uOpacity", 1.0f);

                    ctx.draw_mesh(mr->gpu_mesh_handle(), point_shadow_shader_);
                });
        }

        ++light_idx;
    }

    ctx.set_framebuffer(RHIFramebufferHandle{});
}

void RenderPipeline::bind_point_shadow_uniforms(RenderContext& ctx, RHIShaderHandle shader) {
    if (!shader.is_valid()) return;

    // bind_global_uniforms 会依次绑定 pbr / skinned 两个 shader；
    // 这里必须先把目标 shader 设为当前 program，否则 uniform 位置来自
    // 另一个 program 而当前绑定的是上一个 shader → GL_INVALID_OPERATION。
    ctx.set_shader(shader);

    // 统计点光源数量
    int point_count = 0;
    for (const auto& light : lights_) {
        if (light.type == LightType::Point) ++point_count;
    }
    const int shadow_count = std::min(point_count, static_cast<int>(k_max_point_shadows));

    ctx.set_uniform_int(shader, "uPointShadowCount", shadow_count);

    // 绑定点光源阴影纹理
    if (shadow_count > 0 && !point_shadow_tex_.empty() && point_shadow_tex_[0].is_valid()) {
        ITexture* tex0 = ctx_->texture(point_shadow_tex_[0]);
        if (tex0) tex0->bind(TextureSlots::kPointShadow0);
        ctx.set_texture(shader, point_shadow_tex_[0], TextureSlots::kPointShadow0, "");
        ctx.set_uniform_int(shader, "uPointShadowMap0", TextureSlots::kPointShadow0);
    }
    if (shadow_count > 1 && point_shadow_tex_.size() > 1 && point_shadow_tex_[1].is_valid()) {
        ITexture* tex1 = ctx_->texture(point_shadow_tex_[1]);
        if (tex1) tex1->bind(TextureSlots::kPointShadow1);
        ctx.set_texture(shader, point_shadow_tex_[1], TextureSlots::kPointShadow1, "");
        ctx.set_uniform_int(shader, "uPointShadowMap1", TextureSlots::kPointShadow1);
    }

    // 上传点光源位置和范围
    int point_idx = 0;
    for (int i = 0; i < static_cast<int>(lights_.size()) && point_idx < k_max_point_shadows; ++i) {
        if (lights_[i].type != LightType::Point) continue;
        char name[48];
        std::snprintf(name, sizeof(name), "uPointLightPosWorld[%d]", point_idx);
        ctx.set_uniform_vec3(shader, name, lights_[i].position);
        std::snprintf(name, sizeof(name), "uPointLightRange[%d]", point_idx);
        ctx.set_uniform_float(shader, name, lights_[i].range);
        ++point_idx;
    }
}

} // namespace gryce_engine::render
