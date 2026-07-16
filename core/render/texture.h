#pragma once

#include <cstdint>
#include <string>

namespace gryce_engine::render {

// ---------------------------------------------------------------------------
// TextureFormat — 跨 API 纹理格式
// ---------------------------------------------------------------------------
enum class TextureFormat {
    RGB8,
    RGBA8,
    RGBA16F,
    Depth16,
    Depth24,
    Depth24Stencil8,
    Depth32F,
    R8,
    RG8
};

// ---------------------------------------------------------------------------
// ITexture — 跨 API 纹理接口
// ---------------------------------------------------------------------------
enum class TextureFilter {
    Nearest,
    Linear,
    NearestMipmapNearest,
    LinearMipmapLinear
};

enum class TextureWrap {
    Repeat,
    ClampToEdge,
    ClampToBorder
};

class ITexture {
public:
    virtual ~ITexture() = default;

    virtual bool load_from_file(const std::string& path) = 0;
    virtual bool create_empty(int width, int height, int channels = 4) = 0;
    virtual bool upload_data(const void* data, int width, int height, int channels = 4) = 0;
    virtual bool create_depth(int width, int height) = 0;

    // 上传 cubemap 六个面，顺序固定为 +X,-X,+Y,-Y,+Z,-Z；
    // 每面 width×height×channels，像素按 top-down 行序，不做翻转。
    // 不支持 cubemap 的后端返回 false。
    virtual bool upload_cubemap(const void* faces[6], int width, int height, int channels = 4) {
        (void)faces; (void)width; (void)height; (void)channels;
        return false;
    }

    virtual bool is_cubemap() const { return false; }

    // 显式格式创建（PBR、阴影图、HDR 等场景）
    virtual bool create(TextureFormat format, int width, int height, const void* data = nullptr) = 0;

    virtual void bind(uint32_t slot = 0) const = 0;
    virtual void unbind() const = 0;

    virtual void set_filter(TextureFilter min, TextureFilter mag) = 0;
    virtual void set_wrap(TextureWrap s, TextureWrap t) = 0;

    virtual int width() const = 0;
    virtual int height() const = 0;
    virtual bool is_valid() const = 0;
};

} // namespace gryce_engine::render
