#pragma once

#include "render/framebuffer.h"

namespace gryce_engine::render {

// ---------------------------------------------------------------------------
// GLFramebuffer — OpenGL 帧缓冲实现
// ---------------------------------------------------------------------------
class GLFramebuffer : public IFramebuffer {
public:
    GLFramebuffer();
    ~GLFramebuffer() override;

    bool create(int width, int height) override;
    void destroy() override;

    void bind() const override;
    void unbind() const override;

    void resize(int width, int height) override;

    void attach_color_texture(ITexture* texture) override;
    void attach_depth_texture(ITexture* texture) override;

    int color_sample_count() const override;
    // MSAA resolve：glBlitFramebuffer（READ=本 FBO，DRAW=dst）。
    // GL 规范要求 FBO 内所有附件采样数一致，因此 MSAA 目标的深度附件也是
    // 多样本：这里颜色与深度一起 blit 回单样本（dst 的两个附件分别接收）。
    bool resolve_color_to(IFramebuffer* dst) override;

    bool is_complete() const override;

    int width() const override { return width_; }
    int height() const override { return height_; }

    uint32_t fbo_id() const { return fbo_id_; }
    // 深度附件信息（resolve 决定是否带 GL_DEPTH_BUFFER_BIT）。
    bool has_depth() const { return depth_samples_ > 0; }

private:
    uint32_t fbo_id_ = 0;
    int width_ = 0;
    int height_ = 0;
    int color_samples_ = 1;   // 颜色附件 0 的多样本数
    int depth_samples_ = 0;   // 深度附件多样本数（0 = 无深度附件）
};

} // namespace gryce_engine::render