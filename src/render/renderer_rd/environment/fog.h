#pragma once

#include <cstdint>
#include <string>

#include "render/rhi_handle.h"
#include "math/math.h"

namespace gryce_engine::render {

class RenderContext;

// ---------------------------------------------------------------------------
// VolumetricFog_RD — 体积雾
// 参考 Godot 的 VolumetricFog 实现：
// 使用 3D 纹理的 fog 体积，从深度缓冲重建世界位置，逐体素计算雾密度。
// ---------------------------------------------------------------------------
class VolumetricFog_RD {
public:
    static constexpr int k_fog_resolution_x = 64;  // 水平分辨率
    static constexpr int k_fog_resolution_y = 64;  // 垂直分辨率
    static constexpr int k_fog_resolution_z = 64;  // 深度方向

    VolumetricFog_RD() = default;
    ~VolumetricFog_RD() { destroy(); }

    bool init(RenderContext* ctx, const std::string& shader_dir = "res:/shaders");
    void destroy();

    bool create_targets(int viewport_w, int viewport_h);
    void destroy_targets();

    // 渲染 fog 体积（到内部 3D 纹理）
    void render(RenderContext* ctx,
                RHITextureHandle depth_tex,
                const math::Matrix4f& inv_view_proj,
                const math::Matrix4f& view_matrix,
                const math::Vector3f& camera_pos,
                const math::Vector3f& fog_color,
                float fog_density, float fog_height,
                float fog_near, float fog_far);

    // 合成 fog 到场景颜色
    void render_apply(RenderContext* ctx,
                      RHITextureHandle scene_color_tex,
                      RHITextureHandle depth_tex,
                      const math::Matrix4f& inv_view_proj,
                      const math::Vector3f& camera_pos);

    RHITextureHandle fog_tex() const { return fog_tex_; }
    bool valid() const { return fog_tex_.is_valid(); }

private:
    RenderContext* ctx_ = nullptr;

    // 3D fog 体积纹理（2D 纹理模拟 3D volume，切片水平排列）
    RHITextureHandle fog_tex_;
    RHIFramebufferHandle fog_fbo_;

    // 降采样深度
    RHITextureHandle depth_down_tex_;
    RHIFramebufferHandle depth_down_fbo_;

    // Shader
    RHIShaderHandle fog_shader_;
    RHIShaderHandle fog_apply_shader_;
    RHIMeshHandle fullscreen_mesh_;

    bool initialized_ = false;
};

} // namespace gryce_engine::render