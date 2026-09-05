#pragma once

#include "render/rhi_handle.h"
#include "render/texture.h"
#include "render/framebuffer.h"
#include "render/shader.h"

namespace gryce_engine::render {

class RenderContext;

// ---------------------------------------------------------------------------
// BokehDOF_RD — 六边形 Bokeh 景深效果
// 参考 Godot 的 BokehDOF 实现：
// 1. 半分辨率降采样 + 计算 CoC
// 2. 水平方向六边形 Bokeh 模糊
// 3. 垂直方向六边形 Bokeh 模糊
// 4. 上采样合成到全分辨率
// ---------------------------------------------------------------------------
class BokehDOF_RD {
public:
    BokehDOF_RD() = default;
    ~BokehDOF_RD() { destroy(); }

    bool init(RenderContext* ctx);
    void destroy();

    bool create_targets(int width, int height);
    void destroy_targets();

    void render(RenderContext* ctx,
                RHITextureHandle color_tex,
                RHITextureHandle depth_tex,
                const PostProcessParams& params,
                int viewport_w, int viewport_h);

    RHITextureHandle dof_tex() const { return dof_tex_; }
    bool valid() const { return dof_tex_.is_valid(); }

private:
    RenderContext* ctx_ = nullptr;

    // DOF 缓冲（半分辨率 + 全分辨率）
    int dof_w_ = 0;
    int dof_h_ = 0;

    // 半分辨率 CoC + 颜色缓冲
    RHITextureHandle half_tex_;
    RHIFramebufferHandle half_fbo_;

    // 水平/垂直模糊双缓冲
    RHITextureHandle blur_tex_[2];
    RHIFramebufferHandle blur_fbo_[2];

    // 全分辨率输出
    RHITextureHandle dof_tex_;
    RHIFramebufferHandle dof_fbo_;

    // Shader 句柄
    RHIShaderHandle dof_downsample_shader_;
    RHIShaderHandle dof_bokeh_h_shader_;
    RHIShaderHandle dof_bokeh_v_shader_;
    RHIShaderHandle dof_upsample_shader_;
    RHIMeshHandle fullscreen_mesh_;

    bool initialized_ = false;
};

} // namespace gryce_engine::render