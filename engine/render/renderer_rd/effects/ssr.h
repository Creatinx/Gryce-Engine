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
    // 注意：不能把 targets_valid_ 写进 valid()。管线初始化是
    //   ssr_.init(...);  if (hdr_enabled_ && ssr_.valid()) ssr_.create_targets(...);
    // 而 targets_valid_ 只有 create_targets 成功后才置位 —— 用 valid() 当守卫
    // 会形成死锁：targets 永不创建 → valid() 永远 false → SSR 永不执行。
    // 与 SSIL_RD / Water_RD 保持一致，用 initialized_；targets 的就绪状态
    // 在 render() 内部单独判断（那里确实会 early-out）。
    bool valid() const { return initialized_; }
    bool targets_valid() const { return targets_valid_; }

private:
    RenderContext* ctx_ = nullptr;

    // SSR 输出（全分辨率 RGBA16F）：光线步进结果 + 双边模糊结果
    RHITextureHandle ssr_tex_;
    RHIFramebufferHandle ssr_fbo_;
    RHITextureHandle ssr_tex_blur_;
    RHIFramebufferHandle ssr_blur_fbo_;
    // SSR 合成结果（全分辨率 RGBA16F）：场景色 + 反射。
    // 必须落在独立目标上，再整体拷回 output_fbo：
    //   - 直接画进 output_fbo 会读它自己的颜色附件（自反馈，GL 实测整帧变黑）；
    //   - Vulkan 的 render pass 是 loadOp=CLEAR，重新绑定 HDR 目标会清空场景色，
    //     靠加法混合"叠回"在 Vulkan 上必然丢掉画面。
    RHITextureHandle composite_tex_;
    RHIFramebufferHandle composite_fbo_;

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
    // 合成结果 → output_fbo 的整体拷贝（单输入全屏 pass）
    RHIShaderHandle hdr_copy_shader_;
    RHIMeshHandle fullscreen_mesh_;

    int ssr_w_ = 0;
    int ssr_h_ = 0;

    bool initialized_ = false;
    bool targets_valid_ = false;
};

} // namespace gryce_engine::render
