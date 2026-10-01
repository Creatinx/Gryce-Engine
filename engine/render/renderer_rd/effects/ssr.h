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

    // 渲染 SSR：HiZ 构建 → 光线步进 → 双边模糊 → 合成（场景色 + 反射）
    //
    // 合成结果通过 output_texture() 返回，由调用方把它接成新的 HDR 颜色输入
    //（与 DOF / MotionBlur 的做法一致）。
    //
    // 不再"合成后再整帧拷回 output_fbo"：
    //   - Vulkan 的 render pass 是 loadOp=CLEAR，重新绑定 HDR 目标会把 **深度附件
    //     一起清掉**，于是 SSR 之后所有读深度的效果（SSAO / 接触阴影 / SSIL / 雾）
    //     全部读到远平面 —— GL 与 Vulkan 画面因此不一致（GL 的深度还在）；
    //   - 少一趟全屏拷贝，两个后端行为完全对称。
    // output_fbo 参数保留（调用方语义/兼容），但不再向它写入。
    void render(RenderContext* ctx,
                RHITextureHandle color_tex,
                RHITextureHandle depth_tex,
                RHITextureHandle normal_roughness_tex,
                RHIFramebufferHandle output_fbo,
                const math::Matrix4f& view_matrix,
                const math::Vector3f& camera_pos,
                const PostProcessParams& params,
                int viewport_w, int viewport_h);

    // 反射探针（SSR 出屏兜底）。
    // 传入一张"6 面按 3x2 排布的图集"（RGBA16F，+X,-X,+Y,-Y,+Z,-Z 依次为
    // 上排三格 + 下排三格）。光线步进未命中且不属于原有屏幕空间回退分支时，
    // 按反射方向采样这张图集，避免贴近反射物时退化成一整片环境色（天空）。
    // 传空句柄即可关闭兜底，退回旧行为（交给 IBL）。
    //
    // ready 表示图集本帧已完成至少一次真实捕获：未就绪时着色器不做兜底
    //（否则会采到清屏色，把反射整体压灰），退回旧的"未命中=黑色"行为。
    void set_reflection_probe(RHITextureHandle atlas_tex, bool ready) {
        probe_atlas_ = atlas_tex;
        probe_ready_ = ready;
    }

    // 访问
    RHITextureHandle ssr_tex() const { return ssr_tex_blur_; }
    RHIFramebufferHandle ssr_fbo() const { return ssr_fbo_; }
    // SSR 内部渲染分辨率缩放（0.25~1.0）。由 PostProcessParams 每帧同步，
    // 变化时自动重建目标；合成始终在全分辨率进行。
    float resolution_scale() const { return resolution_scale_; }
    // 合成结果（场景色 + 反射）。调用方应把它当作后续 pass 的 HDR 颜色输入。
    RHITextureHandle output_texture() const { return composite_tex_; }
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
    // 合成目标尺寸（= 视口全分辨率）；ssr_w_/ssr_h_ 是缩放后的步进/模糊尺寸。
    int comp_w_ = 0;
    int comp_h_ = 0;
    float resolution_scale_ = 1.0f;
    bool targets_dirty_ = false;

    bool initialized_ = false;
    bool targets_valid_ = false;

    // 反射探针图集（由 RenderPipeline 每帧设置；空 = 关闭兜底）
    RHITextureHandle probe_atlas_;
    // 图集是否已完成至少一次真实捕获（false 时兜底不生效，避免采到清屏色）
    bool probe_ready_ = false;
};

} // namespace gryce_engine::render
