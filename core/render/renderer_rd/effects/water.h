#pragma once
#include <string>

#include "render/rhi_handle.h"
#include "math/math.h"

namespace gryce_engine::render {

class RenderContext;
class ITexture;
class IFramebuffer;

// ---------------------------------------------------------------------------
// Water_RD — 水面渲染
// 使用 Gerstner 波模拟 + 反射/折射 + 菲涅尔效果
// ---------------------------------------------------------------------------
class Water_RD {
public:
    Water_RD() = default;
    ~Water_RD() { destroy(); }

    bool init(RenderContext* ctx, const std::string& shader_dir = "res:/shaders");
    void destroy();

    // 创建反射/折射/水输出目标（半分辨率反射/折射，全分辨率水输出）
    bool create_targets(int width, int height);
    void destroy_targets();

    // 渲染水面反射（从相机位置反射到水面）
    // 调用者需在调用后渲染场景到当前 FBO
    void render_reflection(RenderContext* ctx,
                           const math::Vector3f& camera_pos,
                           float water_height);

    // 渲染水面网格
    void render_water(RenderContext* ctx,
                      RHITextureHandle reflection_tex,
                      RHITextureHandle refraction_tex,
                      RHITextureHandle depth_tex,
                      const math::Matrix4f& view_proj,
                      const math::Vector3f& camera_pos,
                      float time,
                      float water_height);

    // 水输出纹理（渲染管线最终采样）
    RHITextureHandle water_tex() const { return water_tex_; }
    // 反射纹理（渲染管线渲染场景到反射 FBO）
    RHITextureHandle reflection_tex() const { return reflection_tex_; }
    RHITextureHandle refraction_tex() const { return refraction_tex_; }
    RHIFramebufferHandle reflection_fbo() const { return reflection_fbo_; }
    RHIFramebufferHandle refraction_fbo() const { return refraction_fbo_; }

    // 水面网格
    RHIMeshHandle water_mesh() const { return water_mesh_; }
    RHIShaderHandle water_shader() const { return water_shader_; }

    bool valid() const { return initialized_; }

    // 水面参数
    void set_wave_params(float amplitude, float frequency, float speed, float steepness) {
        wave_amplitude_ = amplitude;
        wave_frequency_ = frequency;
        wave_speed_ = speed;
        wave_steepness_ = steepness;
    }
    void set_water_level(float level) { water_height_ = level; }
    void set_water_color(const math::Vector3f& color) { water_color_ = color; }
    void set_water_params(float amplitude, float frequency, float speed,
                          float steepness, float level,
                          const math::Vector3f& color);

    // 参数
    float wave_amplitude_ = 0.3f;
    float wave_frequency_ = 1.5f;
    float wave_speed_ = 0.5f;
    float wave_steepness_ = 0.3f;
    float water_height_ = 0.0f;
    float water_size_ = 100.0f;
    math::Vector3f water_color_ = math::Vector3f(0.0f, 0.3f, 0.5f);
    float water_foam_amount_ = 0.5f;

private:
    RenderContext* ctx_ = nullptr;

    // 水输出纹理（全分辨率）
    RHITextureHandle water_tex_;
    RHIFramebufferHandle water_fbo_;

    // 反射纹理（半分辨率）
    RHITextureHandle reflection_tex_;
    RHIFramebufferHandle reflection_fbo_;
    int reflection_w_ = 512;
    int reflection_h_ = 512;

    // 折射纹理（半分辨率）
    RHITextureHandle refraction_tex_;
    RHIFramebufferHandle refraction_fbo_;

    // 水输出纹理尺寸
    int water_w_ = 0;
    int water_h_ = 0;

    // 水面网格 (网格细分平面)
    RHIMeshHandle water_mesh_;
    void create_water_mesh();

    // Shader
    RHIShaderHandle water_shader_;
    RHIMeshHandle fullscreen_mesh_;

    bool initialized_ = false;
};

} // namespace gryce_engine::render