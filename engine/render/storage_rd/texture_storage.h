#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "render/export.h"
#include "render/texture.h"
#include "math/math.h"

namespace gryce_engine::render {

// 纹理 RID 句柄
using TextureRID = uint32_t;
constexpr TextureRID k_invalid_texture_rid = 0;

// ---------------------------------------------------------------------------
// RendererTextureStorage — 纹理存储抽象接口
// 管理纹理创建、加载、mipmap、采样器状态。
// ---------------------------------------------------------------------------
class GRYCE_RENDERER_API RendererTextureStorage {
public:
    virtual ~RendererTextureStorage() = default;

    virtual TextureRID texture_create() = 0;
    virtual void texture_free(TextureRID rid) = 0;
    virtual void texture_set_data(TextureRID rid, TextureType type,
                                  TextureFormat format, int width, int height,
                                  int depth, int mipmaps, const void* data) = 0;
    virtual void texture_set_path(TextureRID rid, const std::string& path) = 0;
    virtual std::string texture_get_path(TextureRID rid) const = 0;

    virtual int texture_get_width(TextureRID rid) const = 0;
    virtual int texture_get_height(TextureRID rid) const = 0;
    virtual TextureFormat texture_get_format(TextureRID rid) const = 0;

    virtual void update_buffers() = 0;
};

} // namespace gryce_engine::render