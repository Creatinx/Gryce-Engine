#pragma once

#include "render/rhi_handle.h"
#include "render/shader.h"

namespace gryce_engine::render {

class RenderContext;

// ---------------------------------------------------------------------------
// MotionBlur_RD — 屏幕空间运动模糊
// 1. 从深度重建世界位置，计算屏幕空间运动向量
// 2. 沿运动向量方向做径向模糊（N 次采样后平均）
// ---------------------------------------------------------------------------
class MotionBlur_RD {
public:
    MotionBlur_RD() = default;
    ~MotionBlur_RD() { destroy(); }

    bool init(RenderContext* ctx, const std::string& shader_dir = "res:/shaders");
    void destroy();

    bool create_targets(int width, int height);
    void destroy_targets();

    void render(RenderContext* ctx,
                RHITextureHandle color_tex,
                RHITextureHandle depth_tex,
                RHITextureHandle motion_vectors_tex,  // 外部传入的运动向量（可选）
                const PostProcessParams& params,
                int viewport_w, int viewport_h);

    RHITextureHandle output_tex() const { return blur_tex_; }
    bool valid() const { return blur_tex_.is_valid(); }

private:
    RenderContext* ctx_ = nullptr;

    // 全分辨率运动模糊输出
    RHITextureHandle blur_tex_;
    RHIFramebufferHandle blur_fbo_;

    // 半分辨率运动向量缓冲
    RHITextureHandle mv_tex_;
    RHIFramebufferHandle mv_fbo_;

    // Shader
    RHIShaderHandle motion_blur_shader_;
    RHIShaderHandle motion_vectors_shader_;
    RHIMeshHandle fullscreen_mesh_;

    int blur_w_ = 0;
    int blur_h_ = 0;
    bool initialized_ = false;
    bool targets_valid_ = false;
};

} // namespace gryce_engine::render