#pragma once

#include <cstdint>
#include <string>

#include "render/rhi_handle.h"
#include "render/texture.h"
#include "render/framebuffer.h"
#include "render/shader.h"
#include "math/math.h"

namespace gryce_engine::render {

class RenderContext;
class ITexture;
class IFramebuffer;

// ---------------------------------------------------------------------------
// SSR_RD — 屏幕空间反射
// 使用 Hierarchical Z-Buffer 加速的屏幕空间光线步进。
// 参考 Godot 的 SSR 实现：
//   1. 从深度缓冲构建 HiZ 金字塔（2x2 最小深度下采样，共 4 级）
//   2. 从法线/粗糙度计算镜面反射方向，在 HiZ 中层级步进求命中点
//   3. 深度感知双边模糊平滑步进噪点
//   4. 反射颜色叠加回 HDR 场景颜色（tonemap 前）
// ---------------------------------------------------------------------------
class SSR_RD {
public:
    static constexpr int k_ssr_mip_count = 4;

    SSR_RD() = default;
    ~SSR_RD() { destroy(); }

    bool init(RenderContext* ctx, const std::string& shader_dir);
    void destroy();

    // 创建/销毁 SSR 渲染目标（HiZ 4 级 R32F + SSR 输出/模糊 RGBA16F）
    bool create_targets(int width, int height);
    void destroy_targets();

    // 渲染 SSR：HiZ 构建 → 光线步进 → 双边模糊 → 合成回 output_fbo
    void render(RenderContext* ctx,
                RHITextureHandle color_tex,
                RHITextureHandle depth_tex,
                RHITextureHandle normal_roughness_tex,
                RHIFramebufferHandle output_fbo,
                const math::Matrix4f& view_matrix,
                const math::Vector3f& camera_pos,
                const PostProcessParams& params,
                int viewport_w, int viewport_h);

    // 访问
    RHITextureHandle ssr_tex() const { return ssr_tex_blur_; }
    RHIFramebufferHandle ssr_fbo() const { return ssr_fbo_; }
    bool valid() const { return initialized_ && targets_valid_; }

private:
    RenderContext* ctx_ = nullptr;

    // SSR 输出（全分辨率 RGBA16F）：光线步进结果 + 双边模糊结果
    RHITextureHandle ssr_tex_;
    RHIFramebufferHandle ssr_fbo_;
    RHITextureHandle ssr_tex_blur_;
    RHIFramebufferHandle ssr_blur_fbo_;

    // HiZ 缓冲（R32F，逐级 2x2 最小深度下采样）
    RHITextureHandle hiz_tex_[k_ssr_mip_count];
    RHIFramebufferHandle hiz_fbo_[k_ssr_mip_count];
    int hiz_w_[k_ssr_mip_count] = {};
    int hiz_h_[k_ssr_mip_count] = {};

    // Shader 句柄
    RHIShaderHandle ssr_hiz_shader_;
    RHIShaderHandle ssr_trace_shader_;
    RHIShaderHandle ssr_blur_shader_;
    RHIShaderHandle ssr_composite_shader_;
    RHIMeshHandle fullscreen_mesh_;

    int ssr_w_ = 0;
    int ssr_h_ = 0;

    bool initialized_ = false;
    bool targets_valid_ = false;
};

} // namespace gryce_engine::render
