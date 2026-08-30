#pragma once
#include "render/rhi_handle.h"
#include "math/math.h"
#include <array>
#include <vector>

namespace gryce_engine::render {
class RenderContext;

// ---------------------------------------------------------------------------
// 阴影模式枚举
//   PCF  = 默认百分比邻近滤波（现有 CSM + PCSS 路径）
//   VSM  = Variance Shadow Map（方差阴影贴图，RGBA16F 存深度+深度²）
//   ESM  = Exponential Shadow Map（指数阴影贴图，R16F 存 exp(c*depth)）
// ---------------------------------------------------------------------------
enum class ShadowMode {
    PCF = 0,
    VSM = 1,
    ESM = 2,
};

// ---------------------------------------------------------------------------
// VSMESMShadow — VSM/ESM 阴影方案管理类
//
// 职责：
//   - 创建/销毁 VSM 所需的 RGBA16F 纹理 + FBO（每级联一张）
//   - 提供 VSM 渲染 shader（输出深度+深度²）和 VSM 分离式高斯模糊
//   - 提供 ESM 渲染 shader（输出指数深度）
//   - 集成到 RenderPipeline 中替代默认 PCF shadow pass
// ---------------------------------------------------------------------------
class VSMESMShadow {
public:
    static constexpr int k_max_cascades = 4;
    static constexpr int k_vsm_tex_slot      = 45;  // VSM/ESM 级联 0 纹理 slot
    static constexpr int k_vsm_tex_slot_1    = 46;  // 级联 1
    static constexpr int k_vsm_tex_slot_2    = 47;  // 级联 2
    static constexpr int k_vsm_tex_slot_3    = 48;  // 级联 3

    VSMESMShadow() = default;
    ~VSMESMShadow() { destroy(); }

    // 初始化：存储 context 引用，创建默认 shader
    bool init(RenderContext* ctx);
    void destroy();

    // 设置阴影模式
    void set_shadow_mode(ShadowMode mode) { shadow_mode_ = mode; }
    ShadowMode shadow_mode() const { return shadow_mode_; }

    // VSM: 创建 RGBA16F 纹理（R = depth, G = depth²）
    bool create_vsm_targets(int cascade, int width, int height);
    void destroy_vsm_targets();

    // ESM: 指数参数
    float esm_exponent() const { return esm_exponent_; }
    void set_esm_exponent(float exp) { esm_exponent_ = exp; }

    // 获取 VSM/ESM 纹理和 FBO
    RHITextureHandle vsm_tex(int cascade) const { return vsm_tex_[cascade]; }
    RHIFramebufferHandle vsm_fbo(int cascade) const { return vsm_fbo_[cascade]; }

    // 获取 VSM/ESM 渲染 shader
    RHIShaderHandle vsm_shader() const { return vsm_shader_; }
    RHIShaderHandle esm_shader() const { return esm_shader_; }

    // VSM 模糊 shader
    RHIShaderHandle blur_shader() const { return vsm_blur_h_shader_; }

    // VSM 分离式高斯模糊：水平 → 临时纹理 → 垂直
    void blur_vsm(RenderContext* ctx, int cascade, int width, int height);

    bool valid() const { return initialized_; }

private:
    RenderContext* ctx_ = nullptr;
    ShadowMode shadow_mode_ = ShadowMode::PCF;
    float esm_exponent_ = 40.0f;
    float vsm_blur_scale_ = 1.0f;

    // VSM 级联纹理 + FBO（RGBA16F）
    std::array<RHITextureHandle, k_max_cascades> vsm_tex_;
    std::array<RHIFramebufferHandle, k_max_cascades> vsm_fbo_;

    // VSM 分离模糊临时缓冲（单张，所有级联复用）
    RHITextureHandle blur_temp_tex_;
    RHIFramebufferHandle blur_temp_fbo_;

    // Shader 句柄
    RHIShaderHandle vsm_shader_;         // VSM 渲染 shader（输出 depth + depth²）
    RHIShaderHandle esm_shader_;         // ESM 渲染 shader（输出 exp(c*depth)）
    RHIShaderHandle vsm_blur_h_shader_;  // VSM 模糊（水平/垂直方向由 uniform 控制）

    // 全屏四边形 mesh（用于 blur pass）
    RHIMeshHandle fullscreen_mesh_;

    bool initialized_ = false;
};

} // namespace gryce_engine::render