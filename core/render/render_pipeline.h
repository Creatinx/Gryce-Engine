#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "math/math.h"
#include "render/rhi_handle.h"
#include "render/export.h"
#include "render/shader.h"

namespace gryce_engine {
namespace math { class Camera; }
namespace scene { class Scene; }
namespace assets { struct TextureData; }
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
// RenderPipeline — 前向渲染管线
// Shadow Map -> Skybox -> Forward PBR Lighting（多光源：方向光/点光/聚光，
// 不透明 + 透明两阶段）-> HDR Tone Mapping
// ---------------------------------------------------------------------------
class GRYCE_RENDERER_API RenderPipeline {
public:
    enum class LightType { Directional = 0, Point = 1, Spot = 2 };

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
    void set_shadow_bias(float bias);
    // 阴影贴图分辨率（级联 0），必须在 init() 之前调用
    void set_shadow_map_size(int size);
    int shadow_map_size() const { return cascade_sizes_[0]; }
    bool resize_shadow_map(RenderContext* ctx);
    void set_shadow_enabled(bool enabled) { shadow_enabled_ = enabled; }
    bool shadow_enabled() const { return shadow_enabled_; }
    // 阴影正交盒半径（世界单位），阴影盒跟随相机焦点
    void set_shadow_area(float size) { shadow_area_ = size; }
    void set_cull_disabled(bool disabled) { cull_disabled_ = disabled; }

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
    // 屏幕空间接触阴影（Contact Shadow）：主 pass 后沿方向光方向半分辨率
    // 步进采样深度，补落地悬浮（Peter-Panning）脚底的黑。默认关闭。
    // -----------------------------------------------------------------------
    void set_contact_shadow_enabled(bool enabled) { contact_shadow_enabled_ = enabled; }
    bool contact_shadow_enabled() const { return contact_shadow_enabled_; }
    void set_contact_shadow_params(float strength, float radius_world, int steps);

    // Scene View 网格线开关
    void set_grid_enabled(bool enabled) { grid_enabled_ = enabled; }
    bool grid_enabled() const { return grid_enabled_; }

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
    // 注意：仅 OpenGL 后端使用（Vulkan 下 shader 与 FBO 的 render pass 绑定，
    // 重建需要额外处理，本轮视口输出只在 GL 端启用）。
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

    // 设置 ImGui 后端引用（用于 resize 时 invalidate 旧的 descriptor set 缓存）
    void set_imgui_backend(IImGuiBackend* backend) { imgui_backend_ = backend; }

private:
    RHIShaderHandle load_shader(const std::string& name, RHIFramebufferHandle target, bool color_output, bool post_process,
                                bool skinned = false);
    bool create_cascade_shadow_maps(RenderContext* ctx);

    void begin_shadow_pass(RenderContext& ctx, int cascade);
    void end_shadow_pass(RenderContext& ctx);

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

    // 视锥体：6 个平面 ax+by+cz+d=0，Vector4f 存储 (a,b,c,d)
    struct Frustum {
        math::Vector4f planes[6];
        bool contains_sphere(const math::Vector3f& center, float radius) const;
    };
    Frustum extract_frustum(const math::Matrix4f& vp) const;
    bool is_inside_frustum(const Frustum& frustum, const math::Matrix4f& world_transform,
                           const std::string& mesh_path) const;
    bool is_inside_frustum_skinned(const Frustum& frustum, const math::Matrix4f& world_transform,
                                   const std::string& model_path) const;

    bool create_skybox_mesh(RenderContext* ctx);
    void render_skybox(RenderContext& ctx);

    void upload_ibl_textures(RenderContext& ctx, RHIShaderHandle shader);

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
    std::vector<SkinnedDrawItem> skinned_opaque_items_;
    std::vector<SkinnedDrawItem> skinned_transparent_items_;

    // 连续相同材质跳过重复 bind（按 shader 分开缓存，每帧/每 pass 重置）
    const Material* last_bound_material_pbr_ = nullptr;
    const Material* last_bound_material_skinned_ = nullptr;

    RenderContext* ctx_ = nullptr;
    std::string shader_dir_;

    RHIShaderHandle pbr_shader_;
    RHIShaderHandle shadow_shader_;
    RHIShaderHandle skinned_pbr_shader_;   // 可选：加载失败则蒙皮渲染禁用
    RHIShaderHandle grid_shader_;          // 可选：加载失败则 Scene View 网格线禁用

    // CSM 级联阴影：每级一张 depth texture + FBO（级联 0 兼容旧 shadow_map() 访问）
    std::array<RHITextureHandle, k_max_cascades> shadow_maps_;
    std::array<RHIFramebufferHandle, k_max_cascades> shadow_fbos_;
    std::array<int, k_max_cascades> cascade_sizes_ = {2048, 1024, 512, 512};
    std::array<float, k_max_cascades> cascade_biases_ = {0.0015f, 0.003f, 0.006f, 0.012f};
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
    int debug_view_ = 0;
    PostProcessParams pp_params_;

    bool shadow_enabled_ = true;
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
    bool grid_enabled_ = true;

    // Scene View 网格线
    RHIMeshHandle grid_mesh_;
    bool create_grid_mesh(RenderContext* ctx);
    void render_grid(RenderContext& ctx);
    static constexpr float k_grid_size = 1.0f;
    static constexpr float k_grid_major_every = 10.0f;
    static constexpr float k_grid_fade_start = 30.0f;
    static constexpr float k_grid_fade_end = 100.0f;

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
    RHITextureHandle hdr_depth_;
    RHIFramebufferHandle hdr_fbo_;
    RHIShaderHandle tonemap_shader_;
    RHIMeshHandle fullscreen_mesh_;

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
    bool contact_shadow_enabled_ = true;
    float contact_shadow_strength_ = 0.6f;
    float contact_shadow_radius_ = 0.5f;   // 世界单位
    int contact_shadow_steps_ = 4;
    bool contact_shadow_targets_valid_ = false;

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
    void begin_hdr_forward_pass(RenderContext& ctx);
    void end_hdr_forward_pass(RenderContext& ctx);
    void render_tonemap(RenderContext& ctx);
};

} // namespace gryce_engine::render
