#pragma once

#include <cstdint>

namespace gryce_engine::render {

// 前向声明（避免循环包含）
class ITexture;

// ---------------------------------------------------------------------------
// IFramebuffer — 跨 API 帧缓冲接口
// ---------------------------------------------------------------------------
class IFramebuffer {
public:
    virtual ~IFramebuffer() = default;

    virtual bool create(int width, int height) = 0;
    virtual void destroy() = 0;

    virtual void bind() const = 0;
    virtual void unbind() const = 0;

    virtual void resize(int width, int height) = 0;

    virtual void attach_color_texture(ITexture* texture) = 0;
    virtual void attach_depth_texture(ITexture* texture) = 0;

    virtual bool is_complete() const = 0;

    virtual int width() const = 0;
    virtual int height() const = 0;
};

} // namespace gryce_engine::render
