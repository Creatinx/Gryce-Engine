#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "math/math.h"
#include "render/rhi_handle.h"

namespace gryce_engine {
namespace math { class Camera; }
namespace scene { class Scene; }
} // namespace gryce_engine

namespace gryce_engine::render {

class RenderContext;
class IShader;
class ITexture;
class IFramebuffer;
class IMesh;
class Material;

// ---------------------------------------------------------------------------
// RenderPipeline — 前向渲染管线
// Shadow Map -> Skybox -> Forward PBR Lighting（多光源：方向光/点光/聚光，
// 不透明 + 透明两阶段）-> HDR Tone Mapping
// ---------------------------------------------------------------------------
class RenderPipeline {
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
    void set_shadow_bias(float bias) { shadow_bias_ = bias; }
    void set_shadow_enabled(bool enabled) { shadow_enabled_ = enabled; }
    bool shadow_enabled() const { return shadow_enabled_; }
    // 阴影正交盒半径（世界单位），阴影盒跟随相机焦点
    void set_shadow_area(float size) { shadow_area_ = size; }
    void set_cull_disabled(bool disabled) { cull_disabled_ = disabled; }

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
    void set_exposure(float exposure) { exposure_ = exposure; }
    float exposure() const { return exposure_; }
    void set_tone_map_mode(int mode) { tone_map_mode_ = mode; }
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

    // 重建 HDR / 视口渲染目标（编辑器 Viewport 面板尺寸变化时调用）。
    // 线程约束：调用前必须 pause_render_thread()，调用后 resume_render_thread()。
    // 注意：仅 OpenGL 后端使用（Vulkan 下 shader 与 FBO 的 render pass 绑定，
    // 重建需要额外处理，本轮视口输出只在 GL 端启用）。
    bool resize_render_targets(int width, int height);

private:
    RHIShaderHandle load_shader(const std::string& name, RHIFramebufferHandle target, bool color_output, bool post_process,
                                bool skinned = false);
    bool create_shadow_map(RenderContext* ctx);

    void begin_shadow_pass(RenderContext& ctx);
    void end_shadow_pass(RenderContext& ctx);

    void begin_forward_pass(RenderContext& ctx);
    void end_forward_pass(RenderContext& ctx);

    void update_light_space_matrix();
    void bind_global_uniforms(RenderContext& ctx);
    void upload_lights(RenderContext& ctx, RHIShaderHandle shader);

    bool create_skybox_mesh(RenderContext* ctx);
    void render_skybox(RenderContext& ctx);

    RenderContext* ctx_ = nullptr;
    std::string shader_dir_;

    RHIShaderHandle pbr_shader_;
    RHIShaderHandle shadow_shader_;
    RHIShaderHandle skinned_pbr_shader_;   // 可选：加载失败则蒙皮渲染禁用
    RHIShaderHandle grid_shader_;          // 可选：加载失败则 Scene View 网格线禁用

    RHITextureHandle shadow_map_;
    RHIFramebufferHandle shadow_fbo_;
    int shadow_map_size_ = 2048;
    bool shadow_enabled_ = true;
    float shadow_area_ = 15.0f;
    int shadow_light_index_ = -1;

    math::Camera* camera_ = nullptr;
    std::vector<Light> lights_;
    math::Vector3f ambient_ = math::Vector3f(0.15f, 0.15f, 0.15f);
    math::Matrix4f light_space_matrix_ = math::Matrix4f::identity();

    int viewport_width_ = 1280;
    int viewport_height_ = 720;

    float shadow_bias_ = 0.005f;
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

    // HDR rendering targets
    bool hdr_enabled_ = true;
    float exposure_ = 2.0f;
    int tone_map_mode_ = 1; // 0: none, 1: reinhard, 2: aces
    RHITextureHandle hdr_color_;
    RHITextureHandle hdr_depth_;
    RHIFramebufferHandle hdr_fbo_;
    RHIShaderHandle tonemap_shader_;
    RHIMeshHandle fullscreen_mesh_;

    // 编辑器视口离屏输出（tonemap 后的 LDR 纹理，供 Viewport 面板采样）
    bool viewport_output_enabled_ = false;
    RHITextureHandle viewport_color_;
    RHIFramebufferHandle viewport_fbo_;

    bool create_hdr_target(RenderContext* ctx);
    bool create_viewport_target(RenderContext* ctx);
    bool create_fullscreen_mesh(RenderContext* ctx);
    void begin_hdr_forward_pass(RenderContext& ctx);
    void end_hdr_forward_pass(RenderContext& ctx);
    void render_tonemap(RenderContext& ctx);
};

} // namespace gryce_engine::render
