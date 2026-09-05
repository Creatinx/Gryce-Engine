#pragma once

#include "render/rhi_handle.h"
#include "render/texture.h"
#include "render/framebuffer.h"
#include "render/shader.h"

namespace gryce_engine::render {

class RenderContext;

// ---------------------------------------------------------------------------
// FSR2_RD — AMD FSR 2.0 超分辨率
// 需要运动矢量、深度、曝光、颜色输入。
// 参考 Godot 的 FSR2 集成。
// ---------------------------------------------------------------------------
class FSR2_RD {
public:
    FSR2_RD() = default;
    ~FSR2_RD() { destroy(); }

    bool init(RenderContext* ctx);
    void destroy();

    bool create_targets(int render_w, int render_h, int output_w, int output_h);
    void destroy_targets();

    void render(RenderContext* ctx,
                RHITextureHandle color_tex,
                RHITextureHandle depth_tex,
                RHITextureHandle motion_vectors_tex,
                RHITextureHandle exposure_tex,
                const PostProcessParams& params,
                float jitter_x, float jitter_y);

    RHITextureHandle output_tex() const { return output_tex_; }
    bool valid() const { return output_tex_.is_valid(); }

    // 抖动采样偏移（每帧调用）
    static void get_jitter(int frame_index, int render_w, int render_h,
                           float& jitter_x, float& jitter_y);

private:
    RenderContext* ctx_ = nullptr;

    // FSR2 内部缓冲
    RHITextureHandle output_tex_;
    RHIFramebufferHandle output_fbo_;

    // Shader 句柄（FSR2 的 EASU + RCAS）
    RHIShaderHandle fsr2_easu_shader_;
    RHIShaderHandle fsr2_rcas_shader_;
    RHIMeshHandle fullscreen_mesh_;

    int render_w_ = 0;
    int render_h_ = 0;
    int output_w_ = 0;
    int output_h_ = 0;

    bool initialized_ = false;
};

} // namespace gryce_engine::render