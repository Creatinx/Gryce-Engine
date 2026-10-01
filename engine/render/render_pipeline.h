#pragma once

#include <array>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "math/math.h"
#include "render/rhi_handle.h"
#include "render/export.h"
#include "render/shader.h"
#include "math/camera.h"
#include "render/renderer_rd/effects/bokeh_dof.h"
#include "render/renderer_rd/effects/motion_blur.h"
#include "render/renderer_rd/environment/reflection_probe.h"
#include "render/storage_rd/decal_storage.h"
#include "render/renderer_rd/effects/ssr.h"
#include "render/renderer_rd/environment/fog.h"
#include "render/renderer_rd/effects/water.h"
#include "render/renderer_rd/environment/sdfgi.h"
#include "render/renderer_rd/environment/voxel_gi.h"
#include "render/renderer_rd/effects/ssil.h"
#include "render/renderer_rd/shadow/shadow_atlas.h"
#include "render/storage_rd/light_storage.h"

namespace gryce_engine {
namespace math { class Camera; }
namespace scene { class Scene; }
namespace assets { struct TextureData; }
namespace components { class ParticleSystem3D; }
namespace components { class LineRenderer3D; }
namespace components { class TrailRenderer; }
namespace components { class InstancedMeshRenderer; }
namespace components { class Terrain; }
namespace components { class Billboard; }
namespace components { class TextMesh3D; }
namespace components { class Skybox3D; }
namespace components { class ReflectionProbe; }
namespace components { class LightProbeGroup; }
namespace components { class LODGroup; }
namespace components { class FogVolume; }
namespace components { class VolumetricLight; }
namespace render { struct IBLData; }
} // namespace gryce_engine

namespace gryce_engine::render {

class RenderContext;
class IShader;
class ITexture;
class IFramebuffer;
class IMesh;
class Material;
class IImGuiBackend;

// ---------------------------------------------------------------------------
// 视锥剔除工具（RenderPipeline 与 RenderForwardClustered 共用）
// ---------------------------------------------------------------------------
// 视锥体：6 个平面 ax+by+cz+d=0，Vector4f 存储 (a,b,c,d)
struct GRYCE_RENDERER_API CullFrustum {
    math::Vector4f planes[6];
    bool contains_sphere(const math::Vector3f& center, float radius) const;
};

// 从 view-projection 矩阵提取视锥（要求 GL 风格 NDC：z∈[-1,1]）
GRYCE_RENDERER_API CullFrustum extract_cull_frustum(const math::Matrix4f& vp);

// 世界空间网格包围球：按资源路径缓存本地包围球，避免每帧遍历顶点。
// 返回 false 表示无法计算（资源缺失等），调用方应保守保留物体。
GRYCE_RENDERER_API bool compute_world_mesh_bounds(const std::string& mesh_path,
                                                  const math::Matrix4f& world,
                                                  math::Vector3f& out_center, float& out_radius);
GRYCE_RENDERER_API bool compute_world_skinned_mesh_bounds(const std::string& model_path,
                                                          const math::Matrix4f& world,
                                                          math::Vector3f& out_center, float& out_radius);

// ---------------------------------------------------------------------------
// GI 全局光照模式
// ---------------------------------------------------------------------------
// SSIL 复用同一条 GI 间接光通道（uGITexture/uGIEnabled/uGIIndirectIntensity）
enum class GIMode { None = 0, SDFGI = 1, VoxelGI = 2, SSIL = 3 };

// ---------------------------------------------------------------------------
// RenderPipeline — 前向渲染管线
// Shadow Map -> Skybox -> Forward PBR Lighting（多光源：方向光/点光/聚光，
// 不透明 + 透明两阶段）-> HDR Tone Mapping
// ---------------------------------------------------------------------------
class GRYCE_RENDERER_API RenderPipeline {
public:
    enum class LightType { Directional = 0, Point = 1, Spot = 2 };
    // VSM (Variance Shadow Maps) / ESM (Exponential Shadow Maps)
    enum class ShadowMode { PCF = 0, VSM = 1, ESM = 2 };

    struct Light {
        LightType type = LightType::Directional;
        math::Vector3f position = math::Vector3f::zero();      // 点光/聚光
        math::Vector3f direction = math::Vector3f(0.0f, -1.0f, 0.0f); // 方向光/聚光
        math::Vector3f color = math::Vector3f::one();
        float intensity = 1.0f;
        float range = 10.0f;          // 点光/聚光有效半径
        float spot_angle = 45.0f;     // 聚光外锥角（度）
        float spot_softness = 0.2f;   // 聚光内外锥过渡比例 0~1
    };

    static constexpr int k_max_lights = 8;

    RenderPipeline();
    ~RenderPipeline();

    // 初始化渲染管线：加载 shader、创建 shadow map
    bool init(RenderContext* ctx, const std::string& shader_dir = "res:/shaders");
    void shutdown();

    // 每帧准备
    void set_camera(const math::Camera& camera);
    void set_lights(const std::vector<Light>& lights);
    void set_viewport(int width, int height);
    int viewport_width() const { return viewport_width_; }
    int viewport_height() const { return viewport_height_; }
    void set_shadow_bias(float bias);
    // 阴影贴图分辨率（级联 0），必须在 init() 之前调用
    void set_shadow_map_size(int size);
    int shadow_map_size() const { return cascade_sizes_[0]; }
    bool resize_shadow_map(RenderContext* ctx);
    void set_shadow_enabled(bool enabled) { shadow_enabled_ = enabled; }
    bool shadow_enabled() const { return shadow_enabled_; }
    // 点光源阴影（双抛物面映射 + PCF 软阴影）
    void set_point_shadow_enabled(bool enabled) { point_shadow_enabled_ = enabled; }
    bool point_shadow_enabled() const { return point_shadow_enabled_; }
    void set_point_shadow_size(int size) { point_shadow_size_ = size; }
    int point_shadow_size() const { return point_shadow_size_; }

    // Shadow Atlas 阴影贴图图集（将多个光源阴影打包到一张纹理）
    void set_shadow_atlas_enabled(bool enabled) { shadow_atlas_enabled_ = enabled; }
    bool shadow_atlas_enabled() const { return shadow_atlas_enabled_; }
    // 阴影正交盒半径（世界单位），阴影盒跟随相机焦点
    void set_shadow_area(float size) { shadow_area_ = size; }
    void set_cull_disabled(bool disabled) { cull_disabled_ = disabled; }

    // -----------------------------------------------------------------------
    // VSM / ESM 可选阴影方案
    // -----------------------------------------------------------------------
    void set_shadow_mode(ShadowMode mode) { shadow_mode_ = mode; }
    ShadowMode shadow_mode() const { return shadow_mode_; }
    void set_esm_exponent(float exp) { esm_exponent_ = exp; }
    float esm_exponent() const { return esm_exponent_; }

    // -----------------------------------------------------------------------
    // CSM 级联阴影（Cascaded Shadow Maps）
    // -----------------------------------------------------------------------
    static constexpr int k_max_cascades = 4;
    // 级联数量 1..4（默认 3），分割按 practical split scheme（指数 + 线性插值）
    void set_cascade_count(int count);
    int cascade_count() const { return cascade_count_; }
    // 分割系数：0=线性，1=纯对数分布，默认 0.5
    void set_cascade_split_lambda(float lambda);
    float cascade_split_lambda() const { return cascade_split_lambda_; }
    // 每级阴影贴图分辨率，必须在 init() 之前调用（默认 {2048,1024,512,512}）
    void set_cascade_sizes(const std::array<int, k_max_cascades>& sizes);
    const std::array<int, k_max_cascades>& cascade_sizes() const { return cascade_sizes_; }
    // 每级深度 bias（Slope-Scaled 之后的基础值，默认 {0.0015,0.003,0.006,0.012}）
    void set_cascade_biases(const std::array<float, k_max_cascades>& biases);
    // Normal Offset Shadow Mapping：沿法线把几何推向光源，减少悬浮；1.0=按 texel 自动
    void set_normal_offset_scale(float scale) { normal_offset_scale_ = scale; }

    // -----------------------------------------------------------------------
    // PCSS 软阴影（默认关闭；开启后按遮挡距离动态调整 PCF 半径）
    // -----------------------------------------------------------------------
    void set_pcss_enabled(bool enabled) { pcss_enabled_ = enabled; }
    bool pcss_enabled() const { return pcss_enabled_; }
    void set_pcss_params(float light_size, float max_radius_texels, float tap_scale = 1.0f);

    // -----------------------------------------------------------------------
    // HDR 分析视图：0 Final, 1 Albedo, 2 Normal, 3 Roughness, 4 Metallic,
    // 5 Shadow, 6 Direct, 7 Indirect, 8 Cascade
    // -----------------------------------------------------------------------
    void set_debug_view(int mode) { debug_view_ = mode; }
    int debug_view() const { return debug_view_; }

    // -----------------------------------------------------------------------
    // Tonemap 后处理参数（曝光/曲线/调色/抖动）
    // -----------------------------------------------------------------------
    void set_tonemap_params(const PostProcessParams& params) { pp_params_ = params; }
    const PostProcessParams& tonemap_params() const { return pp_params_; }

    // Bloom 后处理（阈值提取 → 多级降采样模糊 → 上采样合成）
    void set_bloom_enabled(bool enabled) { pp_params_.bloom_enabled = enabled ? 1 : 0; }
    bool bloom_enabled() const { return pp_params_.bloom_enabled != 0; }
    void set_bloom_params(float threshold, float intensity) {
        pp_params_.bloom_threshold = threshold;
        pp_params_.bloom_intensity = intensity;
    }

    // -----------------------------------------------------------------------
    // 3D LUT 色彩分级（1024x32 打包贴图，start() 之前调用；传空字符串清除）
    // -----------------------------------------------------------------------
    void set_color_lut(const std::string& path);
    bool has_color_lut() const { return lut_texture_.is_valid(); }
    void set_lut_enabled(bool enabled) { pp_params_.use_lut = enabled ? 1 : 0; }
    void set_lut_strength(float strength) { pp_params_.lut_strength = strength; }

    // -----------------------------------------------------------------------
    // 自动曝光：GPU 侧亮度反馈（HDR → 亮度链 → 1x1 → 曝光更新），默认关闭
    // -----------------------------------------------------------------------
    void set_auto_exposure(bool enabled) { pp_params_.auto_exposure = enabled ? 1 : 0; }
    bool auto_exposure() const { return pp_params_.auto_exposure != 0; }
    void set_auto_exposure_params(float target_luminance, float min_exposure,
                                  float max_exposure, float speed);

    // -----------------------------------------------------------------------
    // TAA：时域累积 + 半像素抖动 + 邻域钳制（v1，无运动矢量重投影）
    // -----------------------------------------------------------------------
    void set_taa_enabled(bool enabled) { pp_params_.taa_enabled = enabled ? 1 : 0; }
    bool taa_enabled() const { return pp_params_.taa_enabled != 0; }
    void set_taa_weight(float weight) {
        pp_params_.taa_weight = math::clamp(weight, 0.0f, 0.95f);
    }

    // -----------------------------------------------------------------------
    // 物理光照单位：点/聚光按 lumen→candela(÷4π) 换算，方向光按 lux 直传
    // （需配合 EV100/曝光使用，默认关闭）
    // -----------------------------------------------------------------------
    void set_light_units_physical(bool enabled) { physical_light_units_ = enabled; }
    bool light_units_physical() const { return physical_light_units_; }

    // -----------------------------------------------------------------------
    // 屏幕空间环境光遮蔽（GTAO-lite 地平线搜索 + 深度感知双边上模糊，默认关闭）
    // -----------------------------------------------------------------------
    void set_ssao_enabled(bool enabled) { pp_params_.ssao_enabled = enabled ? 1 : 0; }
    bool ssao_enabled() const { return pp_params_.ssao_enabled != 0; }
    void set_ssao_params(float strength, float radius_px) {
        pp_params_.ssao_strength = math::clamp(strength, 0.0f, 2.0f);
        pp_params_.ssao_radius = std::max(1.0f, radius_px);
    }

    // -----------------------------------------------------------------------
    // SSR 屏幕空间反射（Screen Space Reflections，默认关闭）
    // -----------------------------------------------------------------------
    // 注意：ssr_enabled_ 只控制"跑不跑 SSR pass"，SSR_RD 自己还会看
    // pp_params_.ssr_enabled（shader uniform uSSREnabled）——两个都要写，
    // 否则 pass 跑到一半被参数挡掉。参见 set_ssil_enabled 的写法。
    void set_ssr_enabled(bool enabled) {
        ssr_enabled_ = enabled;
        pp_params_.ssr_enabled = enabled ? 1 : 0;
    }
    bool ssr_enabled() const { return ssr_enabled_; }
    void set_ssr_params(float max_steps, float fade_range);
    // SSR 画质参数：步进上限 / 参与反射的粗糙度上限 / 屏幕空间厚度 / 去噪强度。
    // max_steps 上限 128（shader 里循环上限与 push 块尺寸按此约束）。
    void set_ssr_quality(float max_steps, float max_roughness, float thickness,
                         float bilateral) {
        if (max_steps < 8.0f) max_steps = 8.0f;
        if (max_steps > 128.0f) max_steps = 128.0f;
        ssr_max_steps_ = max_steps;
        pp_params_.ssr_max_steps = static_cast<int>(max_steps);
        pp_params_.ssr_max_roughness = math::clamp(max_roughness, 0.0f, 1.0f);
        pp_params_.ssr_thickness = thickness < 0.001f ? 0.001f : thickness;
        pp_params_.ssr_bilateral_filter = math::clamp(bilateral, 0.0f, 1.0f);
    }
    // SSR 无命中时回退到 IBL 环境反射的强度（0~1）。
    void set_ssr_env_fallback(float strength) {
        pp_params_.ssr_env_fallback = math::clamp(strength, 0.0f, 1.0f);
    }
    // SSR 内部步进/模糊分辨率缩放（0.25~1.0，合成始终全分辨率）。
    void set_ssr_resolution_scale(float scale) {
        pp_params_.ssr_resolution_scale = math::clamp(scale, 0.25f, 1.0f);
    }
    // SSR 调试视图模式（shader uniform uSSRDebugMode）。
    void set_ssr_debug_view(int mode) { pp_params_.ssr_debug_view = mode; }

    // -----------------------------------------------------------------------
    // 体积雾（Volumetric Fog，默认关闭）
    // -----------------------------------------------------------------------
    void set_fog_enabled(bool enabled) { fog_enabled_ = enabled; }
    bool fog_enabled() const { return fog_enabled_; }
    void set_fog_params(const math::Vector3f& color, float density, float height);

    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // Bokeh DOF 景深（默认关闭）
    // -----------------------------------------------------------------------
    void set_dof_enabled(bool enabled) {
        dof_enabled_ = enabled;
        pp_params_.dof_enabled = enabled ? 1 : 0;
    }
    bool dof_enabled() const { return dof_enabled_; }
    void set_dof_params(float focus_dist, float focus_range, float blur_amount) {
        pp_params_.dof_focus_distance = focus_dist;
        pp_params_.dof_focus_radius = focus_range;
        pp_params_.dof_blur_amount = blur_amount;
    }

    // -----------------------------------------------------------------------
    // Motion Blur 运动模糊（默认关闭）
    // -----------------------------------------------------------------------
    void set_motion_blur_enabled(bool enabled) {
        motion_blur_enabled_ = enabled;
        pp_params_.motion_blur_enabled = enabled ? 1 : 0;
    }
    bool motion_blur_enabled() const { return motion_blur_enabled_; }
    void set_motion_blur_amount(float amount) {
        motion_blur_amount_ = amount;
        pp_params_.motion_blur_amount = amount;
    }

    // -----------------------------------------------------------------------
    // 屏幕空间接触阴影（Contact Shadow）：主 pass 后沿方向光方向半分辨率
    // 步进采样深度，补落地悬浮（Peter-Panning）脚底的黑。默认关闭。
    // -----------------------------------------------------------------------
    void set_contact_shadow_enabled(bool enabled) { contact_shadow_enabled_ = enabled; }
    bool contact_shadow_enabled() const { return contact_shadow_enabled_; }
    void set_contact_shadow_params(float strength, float radius_world, int steps);

    // -----------------------------------------------------------------------
    // GI 全局光照系统（SDFGI / VoxelGI / SSIL）
    // -----------------------------------------------------------------------
    void set_gi_enabled(bool enabled) { gi_enabled_ = enabled; }
    bool gi_enabled() const { return gi_enabled_; }
    void set_gi_mode(GIMode mode) { gi_mode_ = mode; }
    GIMode gi_mode() const { return gi_mode_; }
    void set_gi_indirect_intensity(float intensity) { gi_indirect_intensity_ = intensity; }
    float gi_indirect_intensity() const { return gi_indirect_intensity_; }
    void set_sdfgi_enabled(bool enabled) { sdfgi_enabled_ = enabled; }
    bool sdfgi_enabled() const { return sdfgi_enabled_; }
    void set_voxel_gi_enabled(bool enabled) { voxel_gi_enabled_ = enabled; }
    bool voxel_gi_enabled() const { return voxel_gi_enabled_; }
    void set_ssil_enabled(bool enabled) {
        ssil_enabled_ = enabled;
        pp_params_.ssil_enabled = enabled ? 1 : 0;
    }
    bool ssil_enabled() const { return ssil_enabled_; }
    // SSIL 强度（间接光整体缩放）
    void set_ssil_intensity(float intensity) { ssil_intensity_ = intensity; }
    float ssil_intensity() const { return ssil_intensity_; }
    // SSIL 采样半径（世界单位）与每根光线的步进数
    void set_ssil_params(float radius, int steps, float intensity) {
        ssil_radius_ = radius < 0.05f ? 0.05f : radius;
        ssil_steps_ = steps < 2 ? 2 : (steps > 32 ? 32 : steps);
        ssil_intensity_ = intensity;
    }

    // 屏幕空间效果（SSAO/SSR/SSIL）当前只在 OpenGL 后端验证可用。
    // Vulkan 侧的屏幕空间链路（深度采样/描述符与 render pass 绑定）尚未打通：
    // 开启后会把整帧压成单色，因此这里显式屏蔽，避免后端产出错误画面。
    bool screen_space_effects_supported() const;
    SDFGI_RD& sdfgi() { return sdfgi_; }
    VoxelGI_RD& voxel_gi() { return voxel_gi_; }
    SSIL_RD& ssil() { return ssil_; }

    // Scene View 网格线开关
    void set_grid_enabled(bool enabled) { grid_enabled_ = enabled; }
    bool grid_enabled() const { return grid_enabled_; }

    // -----------------------------------------------------------------------
    // Water 水面渲染（默认关闭）
    // -----------------------------------------------------------------------
    void set_water_enabled(bool enabled) { water_enabled_ = enabled; }
    bool water_enabled() const { return water_enabled_; }
    void set_water_height(float h) { water_height_ = h; }
    float water_height() const { return water_height_; }
    void set_water_params(float amplitude, float frequency, float speed, float steepness,
                          const math::Vector3f& color, float level = 0.0f) {
        water_.set_water_params(amplitude, frequency, speed, steepness, level, color);
    }
    Water_RD& water_system() { return water_; }

    // -----------------------------------------------------------------------
    // Reflection Probe（反射探针）：局部 IBL 覆盖，当 probe 可用时覆盖全局 IBL
    // -----------------------------------------------------------------------
    void set_probe_system_enabled(bool enabled) { probe_system_enabled_ = enabled; }
    bool probe_system_enabled() const { return probe_system_enabled_; }
    ReflectionProbeRD& probe_system() { return probe_system_; }

    // -----------------------------------------------------------------------
    // Decal 贴花系统：在场景表面投影贴花纹理
    // -----------------------------------------------------------------------
    void set_decal_enabled(bool enabled) { decal_enabled_ = enabled; }
    bool decal_enabled() const { return decal_enabled_; }
    int add_decal(const DecalData& decal) { return decal_storage_.add_decal(decal); }
    void remove_decal(int index) { decal_storage_.remove_decal(index); }
    void update_decal(int index, const DecalData& decal) { decal_storage_.update_decal(index, decal); }
    void clear_decals() { decal_storage_.clear(); }
    DecalStorage& decal_storage() { return decal_storage_; }

    // 环境光（叠加到所有物体的间接光），默认 (0.15, 0.15, 0.15)
    void set_ambient(const math::Vector3f& color) { ambient_ = color; }
    math::Vector3f ambient() const { return ambient_; }

    // 天空盒：按 +X,-X,+Y,-Y,+Z,-Z 顺序传入 6 张贴图路径（res:/ 或绝对路径）。
    // 必须在 RenderContext::start() 之前调用（主线程持有 GPU context）。
    // 传空数组清除天空盒。
    bool set_skybox(const std::array<std::string, 6>& face_paths);
    void clear_skybox();
    bool has_skybox() const { return skybox_texture_.is_valid(); }

    // 环境 HDR/EXR：传入 equirectangular 全景图路径，自动生成 cubemap 与 IBL 资源。
    // 必须在 RenderContext::start() 之前调用（主线程持有 GPU context）。
    // 传空字符串清除。
    bool set_environment_hdr(const std::string& hdr_path);
    // 默认程序化环境（equirect 天空 → IBL + 天空盒回退）。
    // 没有环境贴图时，metallic=1 的材质没有可反射内容、只剩太阳高光，
    // 视觉上就像"材质坏了"；这个接口提供一个开箱可用的环境。
    bool set_default_environment();
    bool default_environment_enabled() const { return environment_procedural_; }
    // 从已设置的天空盒（LDR 六面贴图）派生 IBL 环境（irradiance/prefilter/BRDF）。
    // 未设置天空盒时返回 false。供没有独立 HDR 环境资源的项目使用。
    bool set_environment_from_skybox();
    void clear_environment();
    bool has_environment() const { return ibl_radiance_texture_.is_valid(); }
    void set_ibl_intensity(float intensity) { ibl_intensity_ = intensity; }
    float ibl_intensity() const { return ibl_intensity_; }

    // 渲染一帧：shadow pass -> skybox -> forward PBR（不透明/透明）-> tone mapping
    void render_scene(scene::Scene& scene, RenderContext& ctx);

    // 单独 render 一个 mesh（用于自定义 system）
    void render_mesh(RHIMeshHandle mesh, const Material* material, const math::Matrix4f& model,
                     RenderContext& ctx);

    // -----------------------------------------------------------------------
    // Direct submit 路径（不依赖 Scene/Entity）
    // submit() / submit_instanced() 只入队；render_submitted() 在帧末统一
    // 渲染并清空队列。render_scene() 与 render_submitted() 二选一
    // （按 submit 队列是否非空区分当前走哪个路径）。
    // -----------------------------------------------------------------------
    void submit(IMesh* mesh, const Material* material,
                const math::Matrix4f& transform);
    void submit_instanced(IMesh* mesh, const Material* material,
                          const math::Matrix4f* transforms, int count);
    // submit 路径的相机：position + 组合矩阵 view_proj（GL 风格 z∈[-1,1]）。
    // 内部会从 view_proj 反解出 fov/aspect/near/far 与朝向。
    void set_submit_camera(const math::Vector3f& pos,
                           const math::Matrix4f& view_proj);
    void set_submit_lights(const LightData* lights, int count);
    void set_submit_ambient(const math::Vector3f& color);
    // Renderer 内部调用：渲染所有 submit 的物体（含全部后处理）
    void render_submitted(RenderContext& ctx);
    bool has_submitted_items() const {
        return !submit_items_.empty() || !submit_instances_.empty();
    }
    // 注册 IMesh* → RHIMeshHandle 映射（Renderer 创建/加载网格后调用）
    void register_mesh_mapping(IMesh* ptr, RHIMeshHandle handle);

    // 单独 render 一个蒙皮 mesh：使用 skinned PBR 管线，palette 经
    // set_uniform_mat4_array 推到渲染线程（shared_ptr 按值捕获进命令队列）。
    void render_skinned_mesh(RHIMeshHandle mesh, const Material* material, const math::Matrix4f& model,
                             std::shared_ptr<const std::vector<math::Matrix4f>> palette,
                             RenderContext& ctx);

    // 蒙皮管线是否可用（skinned_pbr shader 加载失败时退化为不可用，不影响普通渲染）
    bool skinning_available() const { return skinned_pbr_shader_.is_valid(); }

    bool is_valid() const { return initialized_; }
    ITexture* shadow_map() const;

    // HDR / Tone mapping 控制
    void set_hdr_enabled(bool enabled) { hdr_enabled_ = enabled; }
    bool hdr_enabled() const { return hdr_enabled_; }
    void set_exposure(float exposure) {
        exposure_ = exposure;
        pp_params_.exposure = exposure;
    }
    float exposure() const { return exposure_; }
    void set_tone_map_mode(int mode) {
        tone_map_mode_ = mode;
        pp_params_.tone_map_mode = mode;
    }
    int tone_map_mode() const { return tone_map_mode_; }

    // -----------------------------------------------------------------------
    // 编辑器视口离屏输出（M1-E1）
    // 开启后 tonemap 结果写入独立 FBO 而非默认 framebuffer，
    // 供编辑器 Viewport 面板以 ImGui::Image 采样；默认 framebuffer 只画 ImGui。
    // 必须在 init() 之前调用。
    // -----------------------------------------------------------------------
    void set_viewport_output_enabled(bool enabled) { viewport_output_enabled_ = enabled; }
    bool viewport_output_enabled() const { return viewport_output_enabled_; }

    // 视口输出纹理（tonemap 后的 LDR 结果）；未启用或创建失败返回 nullptr。
    // 仅读取纹理对象指针/id，主线程调用安全（纹理 id 创建后不可变）。
    ITexture* viewport_color_texture() const;
    // 视口输出纹理的 RHI 句柄（供 SubViewport 等运行时功能注入 2D 组件）
    RHITextureHandle viewport_color_handle() const { return viewport_color_; }

    // 重建 HDR / 视口渲染目标（编辑器 Viewport 面板尺寸变化时调用）。
    // 线程约束：调用前必须 pause_render_thread()，调用后 resume_render_thread()。
    // Vulkan 后端重建后需要配合 hot_reload() 让 pipeline 重新绑定 render pass。
    bool resize_render_targets(int width, int height);

    // -----------------------------------------------------------------------
    // Hot reload: rebuild the whole render pipeline in place (shaders, FBOs,
    // post-process targets) while preserving the current configuration
    // (skybox / IBL / LUT / tonemap / shadow / cascade / viewport output).
    //
    // Safe to call from the main thread while the render thread is running:
    // it pauses the render thread, rebuilds, then resumes it. The caller
    // should invoke this after present() so no unsubmitted commands are lost.
    // Returns true if the rebuild succeeded and the pipeline is usable again.
    // -----------------------------------------------------------------------
    bool hot_reload();

    // 完整重建渲染管线：释放当前 shader / target 后按当前配置重新初始化。
    // 用于加载新项目或场景后 shader 目录/质量设置发生变化时。
    // 线程约束：调用前必须 pause_render_thread()，调用后 resume_render_thread()。
    bool rebuild(RenderContext* ctx, const std::string& shader_dir = "res:/shaders");

    // Shader 热重载：检查本管线持有的 shader 源文件（GLSL/SPIR-V）是否变化，
    // 有变化则 pause_render_thread -> reload() -> resume_render_thread。
    // 调用方应保证在 present() 之后调用（与场景热重载相同的时机约束）。
    // 返回成功重载的 shader 数量。
    int poll_shader_hot_reload(RenderContext& ctx);

    // -----------------------------------------------------------------------
    // 延迟渲染管线（Deferred Rendering）：GBuffer → Lighting Pass
    // 默认关闭，开启后向前渲染切换为 g_buffer → deferred_lighting 流程。
    // -----------------------------------------------------------------------
    void set_deferred_enabled(bool enabled) { deferred_enabled_ = enabled; }
    bool deferred_enabled() const { return deferred_enabled_; }

    // 设置 ImGui 后端引用（用于 resize 时 invalidate 旧的 descriptor set 缓存）
    void set_imgui_backend(IImGuiBackend* backend) { imgui_backend_ = backend; }

private:
    RHIShaderHandle load_shader(const std::string& name, RHIFramebufferHandle target, bool color_output, bool post_process,
                                bool skinned = false);
    bool create_cascade_shadow_maps(RenderContext* ctx);
    // resize_render_targets 的实际实现（重建 HDR / G-Buffer / SSS / SSR / SSIL / SSAO 等目标）。
    bool resize_render_targets_impl(int width, int height);

    void begin_shadow_pass(RenderContext& ctx, int cascade);
    void end_shadow_pass(RenderContext& ctx);
    // 把 shadow bias / 级联 bias / PCSS 参数同步进 PostProcessParams（shader uniform 来源）。
    void sync_shadow_params_to_post_process();

    void begin_forward_pass(RenderContext& ctx);
    void end_forward_pass(RenderContext& ctx);

    void update_light_space_matrix();
    void bind_per_frame_uniforms(RenderContext& ctx, RHIShaderHandle shader);
    void bind_global_uniforms(RenderContext& ctx);
    void upload_lights(RenderContext& ctx, RHIShaderHandle shader);
    // 根据后端类型返回正确 Z 范围的投影矩阵（OpenGL [-1,1] / Vulkan [0,1]）
    math::Matrix4f get_projection_matrix() const;

    void render_mesh_internal(RHIMeshHandle mesh, const Material* material, const math::Matrix4f& model,
                              RenderContext& ctx);
    void render_skinned_mesh_internal(RHIMeshHandle mesh, const Material* material, const math::Matrix4f& model,
                                      std::shared_ptr<const std::vector<math::Matrix4f>> palette,
                                      RenderContext& ctx);
    void render_mesh_to_gbuffer(RHIMeshHandle mesh, const Material* material, const math::Matrix4f& model,
                                RenderContext& ctx);
    void render_skinned_mesh_to_gbuffer(RHIMeshHandle mesh, const Material* material, const math::Matrix4f& model,
                                        std::shared_ptr<const std::vector<math::Matrix4f>> palette,
                                        RenderContext& ctx);

    // 视锥体：6 个平面 ax+by+cz+d=0，Vector4f 存储 (a,b,c,d)
    using Frustum = CullFrustum;
    Frustum extract_frustum(const math::Matrix4f& vp) const;
    bool is_inside_frustum(const Frustum& frustum, const math::Matrix4f& world_transform,
                           const std::string& mesh_path) const;
    bool is_inside_frustum_skinned(const Frustum& frustum, const math::Matrix4f& world_transform,
                                   const std::string& model_path) const;

    bool create_skybox_mesh(RenderContext* ctx);
    void render_skybox(RenderContext& ctx);

    void upload_ibl_textures(RenderContext& ctx, RHIShaderHandle shader);

    // ---- Direct submit 队列 --------------------------------------------------
    struct SubmitItem {
        RHIMeshHandle mesh;
        const Material* material;
        math::Matrix4f model;
    };
    struct SubmitInstanceGroup {
        RHIMeshHandle mesh;
        const Material* material;
        std::vector<math::Matrix4f> transforms;
    };
    std::vector<SubmitItem> submit_items_;
    std::vector<SubmitInstanceGroup> submit_instances_;
    std::unordered_map<const IMesh*, RHIMeshHandle> submitted_mesh_map_;
    // submit 路径内部相机（由 set_submit_camera 从 view_proj 反解填充）
    math::Camera submit_camera_;
    // submit 队列的级联阴影渲染（Shadow Atlas / 点光源阴影需要 Scene 几何，
    // 该路径直接渲染到传统级联 FBO）
    void render_submitted_shadows(RenderContext& ctx);

    // 每帧复用的绘制项容器（clear() 保留 capacity，避免反复堆分配）
    struct DrawItem {
        RHIMeshHandle mesh;
        const Material* material;
        math::Matrix4f model;
        float dist_sq;
    };
    struct SkinnedDrawItem {
        RHIMeshHandle mesh;
        const Material* material;
        math::Matrix4f model;
        std::shared_ptr<const std::vector<math::Matrix4f>> palette;
        float dist_sq;
    };
    std::vector<DrawItem> opaque_items_;
    std::vector<DrawItem> transparent_items_;
    std::vector<DrawItem> viewmodel_items_;
    std::vector<SkinnedDrawItem> skinned_opaque_items_;
    std::vector<SkinnedDrawItem> skinned_transparent_items_;

    // 连续相同材质跳过重复 bind（按 shader 分开缓存，每帧/每 pass 重置）
    const Material* last_bound_material_pbr_ = nullptr;
    const Material* last_bound_material_skinned_ = nullptr;
    const Material* last_bound_material_instanced_ = nullptr;

    RenderContext* ctx_ = nullptr;
    std::string shader_dir_;

    RHIShaderHandle pbr_shader_;
    RHIShaderHandle shadow_shader_;
    RHIShaderHandle shadow_atlas_shader_;  // Shadow Atlas 阴影贴图集 shader
    RHIShaderHandle point_shadow_shader_;  // 点光源双抛物面阴影
    RHIShaderHandle shadow_vsm_shader_;    // VSM 阴影
    RHIShaderHandle shadow_esm_shader_;    // ESM 阴影
    RHIShaderHandle vsm_blur_shader_;      // VSM 模糊
    RHIShaderHandle skinned_pbr_shader_;   // 可选：加载失败则蒙皮渲染禁用
    RHIShaderHandle grid_shader_;          // 可选：加载失败则 Scene View 网格线禁用
    RHIShaderHandle particle_shader_;      // 可选：加载失败则 3D 粒子不绘制
    // 深度补写 pass 专用（高 alpha 阈值，只写可见部分的深度供 SSR 命中）。
    // 加载失败时回退用 particle_shader_，退化为旧的"整块广告牌写深度"行为。
    RHIShaderHandle particle_depth_shader_;
    RHIShaderHandle line3d_shader_;        // 可选：加载失败则 3D 线段/拖尾不绘制
    RHIShaderHandle instanced_shader_;     // 可选：加载失败则 GPU 实例化网格不绘制
    RHIShaderHandle billboard_shader_;     // 可选：加载失败则广告牌（Billboard）不绘制
    RHIShaderHandle text3d_shader_;        // 可选：加载失败则 3D 文本（TextMesh3D）不绘制
    RHIShaderHandle volumelight_shader_;   // 可选：加载失败则体积光柱（VolumetricLight）不绘制

    // CSM 级联阴影：每级一张 depth texture + FBO（级联 0 兼容旧 shadow_map() 访问）
    std::array<RHITextureHandle, k_max_cascades> shadow_maps_;
    std::array<RHIFramebufferHandle, k_max_cascades> shadow_fbos_;
    std::array<int, k_max_cascades> cascade_sizes_ = {2048, 1024, 512, 512};
    std::array<float, k_max_cascades> cascade_biases_ = {0.0005f, 0.001f, 0.002f, 0.004f};
    std::array<math::Matrix4f, k_max_cascades> cascade_light_space_matrices_;
    std::array<float, k_max_cascades + 1> cascade_split_distances_;
    std::array<float, k_max_cascades> cascade_texel_sizes_;
    int cascade_count_ = 3;
    float cascade_split_lambda_ = 0.5f;
    float normal_offset_scale_ = 1.0f;
    bool pcss_enabled_ = false;
    float pcss_light_size_ = 0.05f;
    float pcss_max_radius_ = 16.0f;
    float pcss_tap_scale_ = 1.0f;
    ShadowMode shadow_mode_ = ShadowMode::PCF;
    float esm_exponent_ = 80.0f;
    int debug_view_ = 0;
    PostProcessParams pp_params_;

    bool shadow_enabled_ = true;
    bool shadow_atlas_enabled_ = false;
    ShadowAtlas shadow_atlas_;            // Shadow Atlas 阴影贴图集
    float shadow_area_ = 15.0f;
    int shadow_light_index_ = -1;

    math::Camera* camera_ = nullptr;
    std::vector<Light> lights_;
    math::Vector3f ambient_ = math::Vector3f(0.15f, 0.15f, 0.15f);
    math::Matrix4f light_space_matrix_ = math::Matrix4f::identity(); // 级联 0（兼容/剔除用）

    int viewport_width_ = 1280;
    int viewport_height_ = 720;

    float shadow_bias_ = 0.001f; // 兼容接口：set_shadow_bias 会同步到所有级联
    bool initialized_ = false;
    bool owns_shaders_ = false;
    bool cull_disabled_ = false;
    // Scene View 网格线：默认关闭。它是编辑器辅助显示（地面尺度参照），
    // 若默认开启会随打包游戏一起渲染出去，因此改由编辑器显式打开。
    bool grid_enabled_ = false;

    RHIMeshHandle grid_mesh_;
    bool create_grid_mesh(RenderContext* ctx);
    void render_grid(RenderContext& ctx);
    static constexpr float k_grid_size = 1.0f;
    static constexpr float k_grid_major_every = 10.0f;
    static constexpr float k_grid_fade_start = 30.0f;
    static constexpr float k_grid_fade_end = 100.0f;

    // -----------------------------------------------------------------------
    // 3D 粒子 pass：组件负责 CPU 模拟与顶点流，这里只做收集、状态设置与绘制。
    // collect 必须每帧调用一次、且早于反射探针捕获（探针要把粒子烘进图集）；
    // draw 用调用方所在 pass 的 view/proj，前向 pass 与探针面循环各调一次。
    // -----------------------------------------------------------------------
    void collect_particles(scene::Scene& scene, RenderContext& ctx);
    void draw_particles(RenderContext& ctx, const math::Matrix4f& view, const math::Matrix4f& proj);
    // 只写深度、不改颜色的粒子 pass（blend 用 (ZERO, ONE) 丢弃颜色输出）。
    // SSR 的屏幕空间步进读的是主 HDR 深度，粒子不写深度就永远反射不到；
    // 主粒子绘制必须保持深度写关闭（否则广告牌之间会互相遮挡、破坏加法叠加），
    // 因此单独补这一遍把粒子放进深度缓冲。必须在主粒子绘制之后、SSR 之前调用。
    void draw_particles_depth(RenderContext& ctx, const math::Matrix4f& view, const math::Matrix4f& proj);
    std::vector<components::ParticleSystem3D*> particle_items_;
    // 本帧已启用但还画不出来的粒子系统数（GPU 句柄未就绪 / 尚未攒够发射量）。
    // 反射探针首次捕获要等它归零，否则图集里会缺火焰。
    int particle_pending_count_ = 0;

    // -----------------------------------------------------------------------
    // 3D 线段（折线）与拖尾
    // 与粒子同样拆成 collect / draw：collect 每帧调用一次（组件展开段实例流并
    // 投递上传命令），draw 在前向 pass 的透明阶段与探针面循环各调一次。
    // 拖尾与线段共用 line3d_shader_ 与同一套实例布局，只是数据来源不同。
    // -----------------------------------------------------------------------
    void collect_lines3d(scene::Scene& scene, RenderContext& ctx);
    void draw_lines3d(RenderContext& ctx, const math::Matrix4f& view, const math::Matrix4f& proj);
    std::vector<components::LineRenderer3D*> line_items_;
    std::vector<components::TrailRenderer*> trail_items_;

    // -----------------------------------------------------------------------
    // GPU 实例化网格（植被 / 碎石 / 人群）
    // 同样拆成 collect / draw：collect 每帧调用一次（组件生成实例变换流并投递
    // 上传命令），draw 只在前向 pass 的不透明阶段调用一次。实例化网格是实体
    // 几何（深度写开、背面剔除），因此不走透明阶段。
    // -----------------------------------------------------------------------
    void collect_instanced(scene::Scene& scene, RenderContext& ctx);
    void draw_instanced(RenderContext& ctx);
    std::vector<components::InstancedMeshRenderer*> instanced_items_;

    // -----------------------------------------------------------------------
    // 广告牌（Billboard）与 3D 文本（TextMesh3D）
    // 收集点与粒子/线段相同（每帧一次，早于反射探针捕获），绘制点在前向 pass
    // 的透明阶段（线段之后）。两者都是半透明几何：深度测试开、深度写关。
    //   - Billboard：每帧展开实例流并投递上传命令（朝向由顶点着色器决定）
    //   - TextMesh3D：仅在 text/font/颜色/尺寸变化时重建字形网格（无每帧上传）
    // -----------------------------------------------------------------------
    void collect_billboards(scene::Scene& scene, RenderContext& ctx);
    void draw_billboards(RenderContext& ctx, const math::Matrix4f& view, const math::Matrix4f& proj);
    std::vector<components::Billboard*> billboard_items_;

    void collect_text3d(scene::Scene& scene, RenderContext& ctx);
    void draw_text3d(RenderContext& ctx, const math::Matrix4f& view, const math::Matrix4f& proj);
    struct Text3DItem {
        components::TextMesh3D* text;
        math::Matrix4f model;
    };
    std::vector<Text3DItem> text3d_items_;

    // -----------------------------------------------------------------------
    // FogVolume 组件驱动（纯数据，接入已有全局体积雾 VolumetricFog_RD）
    // collect 每帧调用一次：把启用组件的 color/density/height_falloff/size 归并成
    // 一组全局雾参数（归并规则见 .cpp 注释），至少有一个有效体积时开启体积雾、
    // 一个都没有时关闭；volumetric==false 视为不参与（最省路径 = 不跑雾 pass）。
    // 引擎的体积雾只接受单一全局状态，因此无法表达逐体积的局部雾——这是一处
    // 已记录的能力缺口。场景完全没有 FogVolume 组件时不触碰雾状态。
    // -----------------------------------------------------------------------
    void collect_fog_volumes(scene::Scene& scene);
    // 上次是否已由组件接管雾状态（用于"组件全部关闭后把雾关掉"，不干扰无组件场景）
    bool fog_component_applied_ = false;

    // -----------------------------------------------------------------------
    // VolumetricLight（自包含发射网格光柱）
    // collect 每帧调用一次：组件在参数变化时重建锥体并投递上传命令，这里收集
    // 可绘制项与各自世界变换；draw 在前向 pass 的透明阶段调用（与线段/广告牌同处），
    // 加性混合叠加，深度测试开、深度写关。
    // -----------------------------------------------------------------------
    void collect_volumetric_lights(scene::Scene& scene, RenderContext& ctx);
    void draw_volumetric_lights(RenderContext& ctx, const math::Matrix4f& view,
                                const math::Matrix4f& proj);
    struct VolumetricLightItem {
        components::VolumetricLight* light;
        math::Matrix4f model;
    };
    std::vector<VolumetricLightItem> volumelight_items_;

    // -----------------------------------------------------------------------
    // Skybox3D 组件驱动
    // 组件不新增着色器，只把 texture_path/environment_path/visible/exposure 映射到
    // 已有的天空盒 + IBL 环境 API。这些 API 要求"主线程持有 GPU context"，而
    // start() 之后 context 归渲染线程，因此实际应用动作通过 run_on_render_thread
    // 排进命令流（见 .cpp 中的详细说明）。缓存上次应用的配置，只在变化时重配置，
    // 因为 set_skybox / set_environment_hdr 会重建 cubemap + 预滤波 IBL + 管线。
    // -----------------------------------------------------------------------
    void collect_skybox(scene::Scene& scene, RenderContext& ctx);
    bool skybox_component_applied_ = false;
    bool skybox_component_visible_ = true;
    std::string skybox_component_texture_;
    std::string skybox_component_environment_;
    float skybox_component_exposure_ = 1.0f;
    // upload_ibl_data 内部会 pause_render_thread；当它本身已经运行在渲染线程上时
    // 自我 join 会死锁，用该标志跳过 pause/resume（见 collect_skybox 的应用点说明）。
    bool applying_on_render_thread_ = false;

    // -----------------------------------------------------------------------
    // 地形（程序化高度图网格）
    // collect 每帧调用一次：组件在参数/高度图变化时重建 MeshData 并投递上传
    // 命令。地形是不透明实体几何，复用 pbr_shader_ 与 render_mesh_internal，
    // 因此这里不单独 draw，而是把绘制项并入 opaque_items_（前向与延迟路径
    // 的不透明收集循环各追加一次）。
    // -----------------------------------------------------------------------
    struct TerrainItem {
        components::Terrain* terrain;
        math::Matrix4f model;
    };
    void collect_terrain(scene::Scene& scene, RenderContext& ctx);
    // 把已收集的地形按视锥剔除后追加进不透明绘制列表
    void append_terrain_items(const Frustum& frustum, const math::Vector3f& cam_pos,
                              std::vector<DrawItem>& out) const;
    std::vector<TerrainItem> terrain_items_;

    // -----------------------------------------------------------------------
    // LODGroup（自包含发射网格，与 Terrain 同类）
    // collect 每帧调用一次：组件按相机距离/包围球算出的屏幕覆盖率选 LOD，
    // 加载当前 LOD 网格并投递上传命令；append 把绘制项按视锥剔除后并入
    // 不透明列表（复用 pbr_shader_ 与 render_mesh_internal）。
    // -----------------------------------------------------------------------
    struct LodGroupItem {
        components::LODGroup* lod;
        math::Matrix4f model;
    };
    void collect_lod_groups(scene::Scene& scene, RenderContext& ctx);
    void append_lod_items(const Frustum& frustum, const math::Vector3f& cam_pos,
                          std::vector<DrawItem>& out) const;
    std::vector<LodGroupItem> lod_group_items_;

    // -----------------------------------------------------------------------
    // LightProbeGroup（纯数据，接入间接光路径）
    // collect 每帧调用一次：从管线已缓存的 CPU 侧环境 IBL irradiance 采样出
    // 低频辐照度颜色，乘 intensity 并按探针体与相机的距离衰减，累加进
    // light_probe_ambient_；该值在 bind_per_frame_uniforms 里并入 uAmbient。
    // 无环境（env_irradiance_size_==0）时回退到 ambient_ 作为颜色来源。
    //
    // 注：PBR 着色器在 uUseIBL=1 时用 IBL 结果整体覆盖 ambient（丢弃 uAmbient），
    // 而默认工程项目开启了 IBL，仅写 uAmbient 的贡献会被丢弃。因此这里额外把
    // 贡献亮度折算成 light_probe_ibl_boost_，在 upload_ibl_textures 里乘进
    // uIBLIntensity，保证组件在 IBL 路径下也真实影响帧（详见 .cpp 注释与缺口说明）。
    // -----------------------------------------------------------------------
    void collect_light_probes(scene::Scene& scene, RenderContext& ctx);
    // 从缓存的 irradiance 六面数据按方向采样，估计环境平均辐照度颜色
    math::Vector3f sample_environment_irradiance(int sample_count) const;
    math::Vector3f light_probe_ambient_ = math::Vector3f::zero();
    // 探针贡献折算的 IBL 强度倍增（0 = 无探针贡献，1 = 间接光翻倍）
    float light_probe_ibl_boost_ = 0.0f;
    // upload_ibl_data 中缓存的 CPU 侧 irradiance 六面数据（+X,-X,+Y,-Y,+Z,-Z）
    std::array<std::vector<float>, 6> env_irradiance_faces_;
    int env_irradiance_size_ = 0;

    // -----------------------------------------------------------------------
    // ReflectionProbe（纯数据，接入已有探针系统 ReflectionProbeRD）
    // collect 每帧调用一次：把启用组件的世界位置/立方体范围/强度发布到探针系统
    // 的对应槽位；配置变化或 realtime 节流到期时请求重新捕获。创建/捕获/销毁
    // 都经 ctx.run_on_render_thread 排进渲染线程（create_probe 内含 GPU 资源创建）。
    // 探针可用后由 bind_probe_ibl 覆盖全局 IBL（见 render_mesh_internal）。
    // -----------------------------------------------------------------------
    struct ReflectionProbeBinding {
        components::ReflectionProbe* comp = nullptr;
        bool created = false;   // 主线程侧：是否已请求过 create（槽位按顺序==下标）
        bool captured = false;  // 是否已请求过捕获
        unsigned last_request_tick = 0;
        math::Vector3f last_center;
        math::Vector3f last_extents;
        float last_intensity = 1.0f;
        int last_resolution = 0;
        bool last_realtime = false;
    };
    void collect_reflection_probes(scene::Scene& scene, RenderContext& ctx);
    std::vector<ReflectionProbeBinding> reflection_probe_bindings_;
    // 本帧是否存在启用的 ReflectionProbe 组件（决定是否走探针 IBL 覆盖）
    bool probe_components_active_ = false;
    unsigned probe_reconcile_tick_ = 0;

    // Skybox
    RHITextureHandle skybox_texture_;
    RHIShaderHandle skybox_shader_;
    RHIMeshHandle skybox_mesh_;

    // 供 hot_reload() 重建后重新应用的环境/后期配置
    std::array<std::string, 6> skybox_paths_;
    bool skybox_set_ = false;
    std::string environment_hdr_path_;
    bool environment_set_ = false;
    bool environment_from_skybox_ = false;
    // 是否使用内置程序化环境（热重载后需要重建）
    bool environment_procedural_ = false;
    std::string lut_path_;
    bool lut_set_ = false;

    // IBL (Image-Based Lighting)
    RHITextureHandle ibl_radiance_texture_;
    RHITextureHandle ibl_irradiance_texture_;
    RHITextureHandle ibl_prefilter_texture_;
    RHITextureHandle ibl_brdf_lut_texture_;
    float ibl_intensity_ = 1.0f;
    // 天空盒 CPU 侧 radiance（线性 float，RGBA），供 set_environment_from_skybox 派生 IBL
    std::array<std::vector<float>, 6> skybox_radiance_faces_;
    int skybox_radiance_size_ = 0;
    bool upload_ibl_data(const IBLData& ibl);

    // HDR rendering targets
    bool hdr_enabled_ = true;
    float exposure_ = 1.0f;
    int tone_map_mode_ = 1; // 0: none, 1: reinhard, 2: aces
    RHITextureHandle hdr_color_;
    // G-buffer / deferred pass 期间 hdr_color_ 会被临时指向别的目标；这里保存原本的
    // HDR 颜色目标句柄，pass 结束后恢复（见 postprocess_pass.cpp 的 begin_gbuffer_pass）。
    RHITextureHandle hdr_original_color_;
    RHITextureHandle hdr_depth_;
    RHIFramebufferHandle hdr_fbo_;
    RHIShaderHandle tonemap_shader_;
    RHIMeshHandle fullscreen_mesh_;

    // Deferred Rendering：GBuffer（3x MRT 彩色 + 1x 深度）
    bool deferred_enabled_ = false;
    bool gbuffer_targets_valid_ = false;
    RHITextureHandle gbuffer_albedo_metallic_;   // RT0: RGB=albedo, A=metallic
    RHITextureHandle gbuffer_normal_roughness_;  // RT1: RGB=normal(enc), A=roughness
    RHITextureHandle gbuffer_emissive_ao_;       // RT2: RGB=emissive, A=AO
    RHITextureHandle gbuffer_depth_;             // Depth
    RHIFramebufferHandle gbuffer_fbo_;
    RHIShaderHandle gbuffer_shader_;

    // Depth+Normal 预通道（前向路径）：前向没有 G-buffer，SSR/SSIL 需要的
    // 每像素法线+粗糙度由它填进 gbuffer_normal_roughness_（同一块 RT1 语义）。
    // deferred 路径由 GBuffer pass 填，两者互斥：deferred 开启时不跑预通道。
    RHITextureHandle prepass_normal_depth_;      // 预通道自带的深度，保证遮挡正确
    RHIFramebufferHandle prepass_normal_fbo_;
    RHIShaderHandle depth_normal_shader_;
    RHIShaderHandle deferred_lighting_shader_;

    // Bloom：D 链（阈值 + 降采样模糊）+ U 链（上采样合成），全部半分辨率
    static constexpr int k_bloom_levels = 5;
    std::array<RHITextureHandle, k_bloom_levels> bloom_down_tex_;
    std::array<RHITextureHandle, k_bloom_levels> bloom_up_tex_;
    std::array<RHIFramebufferHandle, k_bloom_levels> bloom_down_fbo_;
    std::array<RHIFramebufferHandle, k_bloom_levels> bloom_up_fbo_;
    std::array<int, k_bloom_levels> bloom_level_w_;
    std::array<int, k_bloom_levels> bloom_level_h_;
    RHIShaderHandle bloom_threshold_shader_;
    RHIShaderHandle bloom_downsample_shader_;
    RHIShaderHandle bloom_upsample_shader_;
    bool bloom_targets_valid_ = false;

    bool create_bloom_targets(RenderContext* ctx);
    void destroy_bloom_targets();
    void render_bloom(RenderContext& ctx);

    // 3D LUT
    RHITextureHandle lut_texture_;
    bool create_lut_texture(RenderContext* ctx, const assets::TextureData* data);

    // 自动曝光：亮度链（6 级，末级 1x1）+ 双缓冲曝光值
    static constexpr int k_lum_levels = 6;
    std::array<RHITextureHandle, k_lum_levels> lum_tex_;
    std::array<RHIFramebufferHandle, k_lum_levels> lum_fbo_;
    std::array<int, k_lum_levels> lum_w_;
    std::array<int, k_lum_levels> lum_h_;
    std::array<RHITextureHandle, 2> exposure_tex_;
    std::array<RHIFramebufferHandle, 2> exposure_fbo_;
    int exposure_ping_ = 0;
    int current_exposure_idx_ = 0;
    RHIShaderHandle lum_average_shader_;
    RHIShaderHandle exposure_update_shader_;
    bool auto_exposure_targets_valid_ = false;

    bool create_auto_exposure_targets(RenderContext* ctx);
    void destroy_auto_exposure_targets();
    void render_auto_exposure(RenderContext& ctx);

    // TAA：双缓冲历史/结果
    std::array<RHITextureHandle, 2> taa_tex_;
    std::array<RHIFramebufferHandle, 2> taa_fbo_;
    int taa_ping_ = 0;
    uint32_t taa_frame_ = 0;
    RHIShaderHandle taa_resolve_shader_;
    bool taa_targets_valid_ = false;

    bool create_taa_targets(RenderContext* ctx);
    void destroy_taa_targets();
    void render_taa(RenderContext& ctx);
    static float halton(uint32_t index, uint32_t base);

    // 物理光照单位
    bool physical_light_units_ = false;

    // GTAO/SSAO：半分辨率 AO + 模糊双缓冲
    std::array<RHITextureHandle, 2> ssao_tex_;
    std::array<RHIFramebufferHandle, 2> ssao_fbo_;
    // 禁用 SSAO 时绑定的合法回退纹理（1x1 白），避免 Vulkan 采样
    // 从未渲染过的 ssao 目标（layout 仍为 UNDEFINED → 验证层报错）
    RHITextureHandle ssao_fallback_tex_;

    // 屏幕空间接触阴影
    RHITextureHandle contact_shadow_tex_;
    RHIFramebufferHandle contact_shadow_fbo_;
    RHIShaderHandle contact_shadow_shader_;
    int cs_w_ = 0;
    int cs_h_ = 0;
    // 临时置为 false：接触阴影在 tonemap 里对所有画面统一做一次乘积，
    // 物体会在接地面周围压出一圈软暗渐变，形态与 SSAO 无关（开关 SSAO 都在），
    // 需要用 A/B 对比确认它是否就是那个"阴影问题"。
    // 确认完记得改回 true（或改成从 project.data 读，别长期写死）。
    bool contact_shadow_enabled_ = false;
    float contact_shadow_strength_ = 0.6f;
    float contact_shadow_radius_ = 0.5f;   // 世界单位
    int contact_shadow_steps_ = 4;
    bool contact_shadow_targets_valid_ = false;

    // SSR 屏幕空间反射
    SSR_RD ssr_;
    bool ssr_enabled_ = false;
    float ssr_max_steps_ = 64;
    float ssr_fade_range_ = 10.0f;

    // 体积雾（Volumetric Fog）
    VolumetricFog_RD fog_;
    bool fog_enabled_ = false;
    math::Vector3f fog_color_ = math::Vector3f(0.5f, 0.5f, 0.5f);
    float fog_density_ = 0.01f;
    float fog_height_ = 20.0f;

    // 水面渲染（Water）
    Water_RD water_;
    bool water_enabled_ = false;
    float water_height_ = 0.0f;
    float water_time_ = 0.0f;

    // Bokeh DOF 景深
    BokehDOF_RD dof_;
    bool dof_enabled_ = false;
    float dof_focus_distance_ = 10.0f;
    float dof_focus_range_ = 5.0f;
    float dof_blur_amount_ = 4.0f;

    // Motion Blur 运动模糊
    MotionBlur_RD motion_blur_;
    bool motion_blur_enabled_ = false;
    float motion_blur_amount_ = 0.5f;

    // GI 全局光照系统
    SDFGI_RD sdfgi_;
    VoxelGI_RD voxel_gi_;
    SSIL_RD ssil_;
    bool gi_enabled_ = false;
    GIMode gi_mode_ = GIMode::None;
    float gi_indirect_intensity_ = 1.0f;
    bool sdfgi_enabled_ = false;
    // SDFGI 每帧方向光收集缓冲：跨帧保留容量，避免每帧堆分配。
    std::vector<math::Vector3f> sdfgi_light_dirs_;
    std::vector<math::Vector3f> sdfgi_light_colors_;
    bool voxel_gi_enabled_ = false;
    bool ssil_enabled_ = false;
    // SSIL 参数：采样半径（世界单位）/ 每根光线步进数 / 强度
    float ssil_radius_ = 0.6f;
    int   ssil_steps_ = 8;
    float ssil_intensity_ = 1.0f;

    // 点光源阴影（双抛物面映射）
    static constexpr int k_max_point_shadows = 4;
    static constexpr int k_default_point_shadow_size = 512;
    int point_shadow_size_ = k_default_point_shadow_size;
    bool point_shadow_enabled_ = true;
    std::vector<RHITextureHandle> point_shadow_tex_;       // 每点光源 1 张纹理
    std::vector<RHIFramebufferHandle> point_shadow_fbo_;    // 每点光源 1 个 FBO（双抛物面共享）
    bool create_point_shadow_targets(RenderContext* ctx);
    void destroy_point_shadow_targets();
    void render_point_shadows(RenderContext& ctx, scene::Scene& scene);
    void bind_point_shadow_uniforms(RenderContext& ctx, RHIShaderHandle shader);

    // VSM/ESM 彩色阴影目标（RGBA16F / R16F）
    std::array<RHITextureHandle, k_max_cascades> vsm_color_tex_;
    std::array<RHIFramebufferHandle, k_max_cascades> vsm_color_fbo_;
    // VSM 模糊输出（与 vsm_color_tex_ 同尺寸）。模糊必须"读原图 → 写这张"，
    // 原地读写同一张纹理在 GL 里是未定义行为。
    std::array<RHITextureHandle, k_max_cascades> vsm_blur_tex_;
    std::array<RHIFramebufferHandle, k_max_cascades> vsm_blur_fbo_;
    bool create_vsm_color_targets(RenderContext* ctx);
    void destroy_vsm_color_targets();
    void render_vsm_blur(RenderContext& ctx);
    // 供采样方取用：VSM 模式下取模糊后的图，否则取原始图。
    RHITextureHandle vsm_shadow_texture(int cascade) const {
        if (shadow_mode_ == ShadowMode::VSM && cascade >= 0 && cascade < k_max_cascades &&
            vsm_blur_tex_[cascade].is_valid()) {
            return vsm_blur_tex_[cascade];
        }
        return vsm_color_tex_[cascade];
    }

    int ssao_w_ = 0;
    int ssao_h_ = 0;
    RHIShaderHandle gtao_shader_;
    RHIShaderHandle ssao_blur_shader_;
    bool ssao_targets_valid_ = false;

    bool create_ssao_targets(RenderContext* ctx);
    void destroy_ssao_targets();
    void render_ssao(RenderContext& ctx);

    bool create_contact_shadow_targets(RenderContext* ctx);
    void destroy_contact_shadow_targets();
    void render_contact_shadow(RenderContext& ctx);

    // 编辑器视口离屏输出（tonemap 后的 LDR 纹理，供 Viewport 面板采样）
    bool viewport_output_enabled_ = false;
    RHITextureHandle viewport_color_;
    RHIFramebufferHandle viewport_fbo_;
    IImGuiBackend* imgui_backend_ = nullptr;

    bool create_hdr_target(RenderContext* ctx);
    bool create_viewport_target(RenderContext* ctx);
    bool create_fullscreen_mesh(RenderContext* ctx);
    bool create_gbuffer_targets(RenderContext* ctx);
    void destroy_gbuffer_targets();

    // Depth+Normal 预通道（前向路径的 SSR/SSIL 法线来源）
    bool create_normal_prepass(RenderContext* ctx);
    void destroy_normal_prepass();
    void render_normal_prepass(scene::Scene& scene, RenderContext& ctx, const Frustum& frustum);

    // SSIL（屏幕空间间接光）—— 相机参数与强度/半径/步数统一在这里下发，
    // 三条渲染路径共用。
    void render_ssil(RenderContext& ctx);
    void begin_gbuffer_pass(RenderContext& ctx);
    void end_gbuffer_pass(RenderContext& ctx);
    void begin_deferred_lighting_pass(RenderContext& ctx);
    void end_deferred_lighting_pass(RenderContext& ctx);
    void render_deferred_lighting_to_hdr(RenderContext& ctx);
    void begin_hdr_forward_pass(RenderContext& ctx);
    void end_hdr_forward_pass(RenderContext& ctx);
    void render_tonemap(RenderContext& ctx);

    // -----------------------------------------------------------------------
    // Reflection Probe / Decal 成员
    // -----------------------------------------------------------------------
    ReflectionProbeRD probe_system_;
    bool probe_system_enabled_ = false;

    DecalStorage decal_storage_;
    bool decal_enabled_ = false;
    RHIShaderHandle decal_shader_;
    RHIMeshHandle decal_box_mesh_;

    bool create_decal_box_mesh(RenderContext* ctx);
    void render_decal_forward(RenderContext& ctx);
    void render_decal_deferred(RenderContext& ctx);
    void bind_probe_ibl(RenderContext& ctx, RHIShaderHandle shader, const math::Vector3f& position);

    // -----------------------------------------------------------------------
    // SSR 出屏兜底用的反射探针（相机锚定的 3x2 面图集）
    // 6 个面（+X,-X,+Y,-Y,+Z,-Z）排在一张 3x2 的 2D 纹理里，规避 RHI 不能
    // 渲染到 cubemap 面的限制。首帧或相机移动超过阈值时从相机位置重绘一次，
    // 结果供 SSR 未命中射线按反射方向采样，解决"贴近反射物时反射不到周围
    // 物体（退化成天空色）"。面的朝向与 ssr_trace.frag 的 probe_sample 对应。
    // -----------------------------------------------------------------------
    // 反射探针图集：6 个面按 3x2 排布，采样时用方向查表（见 ssr_trace.frag 的
    // probe_sample）。面尺寸只影响"SSR 未命中时的环境兜底"精度，1024 面时单次
    // 捕获要重绘 6x1024x1024 像素，实测约 14ms，而它每 16 帧就被活跃粒子触发
    // 一次，是明显的周期性掉帧源。压到 256 后捕获代价降到约 1ms，兜底反射的
    // 精度对屏幕空间反射兜底而言完全够用。
    // 注意：改动此值必须同步 ssr_trace.frag / vulkan_ssr_trace.frag 里的
    // kProbeFaceSize 常量，否则图集采样坐标会错位。
    static constexpr int k_probe_face_size = 256;
    static constexpr int k_probe_atlas_w = k_probe_face_size * 3;
    static constexpr int k_probe_atlas_h = k_probe_face_size * 2;
    bool create_probe_targets(RenderContext* ctx);
    void destroy_probe_targets();
    void capture_reflection_probe(RenderContext& ctx, scene::Scene& scene);
    RHITextureHandle probe_atlas_tex_;
    RHITextureHandle probe_depth_tex_;
    RHIFramebufferHandle probe_fbo_;
    bool probe_targets_valid_ = false;
    bool probe_ready_ = false;
    // 首次抓取前等待网格上传完成的帧数（上传是排队命令，首帧通常尚未完成）
    int probe_defer_frames_ = 0;
    math::Vector3f probe_captured_pos_;
    // 场景内容指纹（网格/材质/世界变换的量化摘要）。相机静止时内容变化
    // （增删实体、换网格、改材质、移动物体）也要重抓，否则反射里会一直
    // 保留旧场景。量化到 1/128 以抑制浮点抖动造成的无谓重抓。
    std::size_t probe_scene_hash_ = 0;
    // 内容变化触发的重抓节流计数（避免逐帧动画导致每帧 6 次全场景捕获）
    unsigned probe_content_tick_ = 0;
    // 累积位移：缓慢靠近镜面时单帧位移可能远小于重抓阈值（如 0.02m/帧），若只看
    // 瞬时阈值，探针会一直停在上一次触发点，近距离反射的方位随视差逐渐失真。
    // 累积到阈值即重抓，保证"慢慢贴近物体"时探针仍跟随相机。
    float probe_drift_accum_ = 0.0f;
};

} // namespace gryce_engine::render
