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

    bool is_valid() const { return initialized_; }
    ITexture* shadow_map() const;

    // HDR / Tone mapping 控制
    void set_hdr_enabled(bool enabled) { hdr_enabled_ = enabled; }
    bool hdr_enabled() const { return hdr_enabled_; }
    void set_exposure(float exposure) { exposure_ = exposure; }
    float exposure() const { return exposure_; }
    void set_tone_map_mode(int mode) { tone_map_mode_ = mode; }
    int tone_map_mode() const { return tone_map_mode_; }

private:
    RHIShaderHandle load_shader(const std::string& name, RHIFramebufferHandle target, bool color_output, bool post_process);
    bool create_shadow_map(RenderContext* ctx);

    void begin_shadow_pass(RenderContext& ctx);
    void end_shadow_pass(RenderContext& ctx);

    void begin_forward_pass(RenderContext& ctx);
    void end_forward_pass(RenderContext& ctx);

    void update_light_space_matrix();
    void bind_global_uniforms(RenderContext& ctx);
    void upload_lights(RenderContext& ctx);

    bool create_skybox_mesh(RenderContext* ctx);
    void render_skybox(RenderContext& ctx);

    RenderContext* ctx_ = nullptr;
    std::string shader_dir_;

    RHIShaderHandle pbr_shader_;
    RHIShaderHandle shadow_shader_;

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

    // Skybox
    RHITextureHandle skybox_texture_;
    RHIShaderHandle skybox_shader_;
    RHIMeshHandle skybox_mesh_;

    // HDR rendering targets
    bool hdr_enabled_ = true;
    float exposure_ = 1.0f;
    int tone_map_mode_ = 1; // 0: none, 1: reinhard, 2: aces
    RHITextureHandle hdr_color_;
    RHITextureHandle hdr_depth_;
    RHIFramebufferHandle hdr_fbo_;
    RHIShaderHandle tonemap_shader_;
    RHIMeshHandle fullscreen_mesh_;

    bool create_hdr_target(RenderContext* ctx);
    bool create_fullscreen_mesh(RenderContext* ctx);
    void begin_hdr_forward_pass(RenderContext& ctx);
    void end_hdr_forward_pass(RenderContext& ctx);
    void render_tonemap(RenderContext& ctx);
};

} // namespace gryce_engine::render
