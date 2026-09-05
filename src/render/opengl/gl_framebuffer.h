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

    bool is_complete() const override;

    int width() const override { return width_; }
    int height() const override { return height_; }

    uint32_t fbo_id() const { return fbo_id_; }

private:
    uint32_t fbo_id_ = 0;
    int width_ = 0;
    int height_ = 0;
};

} // namespace gryce_engine::render