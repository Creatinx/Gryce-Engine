#pragma once

#include <cstdint>
#include "render/texture.h"

namespace gryce_engine::render {

// ---------------------------------------------------------------------------
// GLTexture — OpenGL 纹理实现
// ---------------------------------------------------------------------------
class GLTexture : public ITexture {
public:
    GLTexture();
    ~GLTexture() override;

    bool load_from_file(const std::string& path) override;
    bool create_empty(int width, int height, int channels = 4) override;
    bool upload_data(const void* data, int width, int height, int channels = 4) override;
    bool upload_cubemap(const void* faces[6], int width, int height, int channels = 4) override;
    bool is_cubemap() const override { return is_cubemap_; }
    bool create_depth(int width, int height) override;
    bool create(TextureFormat format, int width, int height, const void* data = nullptr) override;

    bool create_compressed(TextureFormat format, int width, int height,
                           int mip_levels, const void* const* mip_data,
                           const size_t* mip_sizes) override;

    void bind(uint32_t slot = 0) const override;
    void unbind() const override;

    void set_filter(TextureFilter min, TextureFilter mag) override;
    void set_wrap(TextureWrap s, TextureWrap t) override;

    int width() const override { return width_; }
    int height() const override { return height_; }
    bool is_valid() const override { return texture_id_ != 0; }

    uint32_t texture_id() const { return texture_id_; }

private:
    uint32_t texture_id_ = 0;
    int width_ = 0;
    int height_ = 0;
    int channels_ = 4;
    bool is_cubemap_ = false;

    static uint32_t to_gl_filter(TextureFilter filter);
    static uint32_t to_gl_wrap(TextureWrap wrap);
};

} // namespace gryce_engine::render