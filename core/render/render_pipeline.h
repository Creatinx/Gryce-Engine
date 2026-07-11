#pragma once

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
// 支持：Shadow Map -> Forward PBR Lighting
// ---------------------------------------------------------------------------
class RenderPipeline {
public:
    struct Light {
        math::Vector3f direction = math::Vector3f(0.0f, -1.0f, 0.0f);
        math::Vector3f color = math::Vector3f::one();
        float intensity = 1.0f;
    };

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
    void set_cull_disabled(bool disabled) { cull_disabled_ = disabled; }

    // 渲染一帧：先 shadow pass，再 forward pass 到 HDR target，最后 tone mapping 到 backbuffer
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

    RenderContext* ctx_ = nullptr;
    std::string shader_dir_;

    RHIShaderHandle pbr_shader_;
    RHIShaderHandle shadow_shader_;

    RHITextureHandle shadow_map_;
    RHIFramebufferHandle shadow_fbo_;
    int shadow_map_size_ = 2048;

    math::Camera* camera_ = nullptr;
    std::vector<Light> lights_;
    math::Matrix4f light_space_matrix_ = math::Matrix4f::identity();

    int viewport_width_ = 1280;
    int viewport_height_ = 720;

    float shadow_bias_ = 0.005f;
    bool initialized_ = false;
    bool owns_shaders_ = false;
    bool cull_disabled_ = false;

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
