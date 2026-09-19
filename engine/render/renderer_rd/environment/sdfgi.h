#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "render/rhi_handle.h"
#include "math/math.h"

namespace gryce_engine::render {

class RenderContext;

// ---------------------------------------------------------------------------
// SDFGI_RD — 有符号距离场全局光照
// 参考 Godot 的 SDFGI 实现：
// 1. 体素化场景到 SDF
// 2. 动态 GI 级联（每帧注入光照）
// 3. 渲染时从 SDF 采样间接光照
// ---------------------------------------------------------------------------
class SDFGI_RD {
public:
    static constexpr int k_cascade_count = 4;
    static constexpr int k_cascade_size = 64; // 每级级联的体素分辨率

    SDFGI_RD() = default;
    ~SDFGI_RD() { destroy(); }

    bool init(RenderContext* ctx);
    void destroy();

    bool create_cascades(const math::Vector3f& center, float max_distance);
    void destroy_cascades();

    // 更新 SDFGI（每帧调用）
    void update(const math::Vector3f& camera_pos,
                const std::vector<math::Vector3f>& light_directions,
                const std::vector<math::Vector3f>& light_colors);

    RHITextureHandle sdf_texture() const { return sdf_tex_; }
    RHITextureHandle gi_texture() const { return gi_tex_; }
    bool valid() const { return sdf_tex_.is_valid(); }

private:
    struct Cascade {
        math::Vector3f center;
        float size;
        RHITextureHandle sdf_tex;
        RHITextureHandle gi_tex;
        RHITextureHandle irradiance_tex;
        RHIFramebufferHandle fbo;
    };

    RenderContext* ctx_ = nullptr;
    Cascade cascades_[k_cascade_count];
    RHITextureHandle sdf_tex_;
    RHITextureHandle gi_tex_;
    bool initialized_ = false;
};

} // namespace gryce_engine::render