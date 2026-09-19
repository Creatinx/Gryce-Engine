#pragma once

#include "render/storage_rd/texture_storage.h"
#include "render/rhi_handle.h"
#include <unordered_map>
#include <string>

namespace gryce_engine::render {

class RenderContext;

// ---------------------------------------------------------------------------
// TextureStorageImpl — 纹理存储实现
// 管理纹理创建、加载、mipmap、采样器状态。
// ---------------------------------------------------------------------------
class TextureStorageImpl : public RendererTextureStorage {
public:
    TextureStorageImpl(RenderContext* ctx);
    ~TextureStorageImpl() override;

    TextureRID texture_create() override;
    void texture_free(TextureRID rid) override;
    void texture_set_data(TextureRID rid, TextureType type,
                          TextureFormat format, int width, int height,
                          int depth, int mipmaps, const void* data) override;
    void texture_set_path(TextureRID rid, const std::string& path) override;
    std::string texture_get_path(TextureRID rid) const override;

    int texture_get_width(TextureRID rid) const override;
    int texture_get_height(TextureRID rid) const override;
    TextureFormat texture_get_format(TextureRID rid) const override;

    void update_buffers() override;

private:
    struct TextureData {
        RHITextureHandle gpu_handle;
        TextureType type = TextureType::Texture2D;
        TextureFormat format = TextureFormat::RGBA8;
        int width = 0;
        int height = 0;
        int depth = 1;
        int mipmaps = 1;
        std::string path;
        bool dirty = false;
        const void* pending_data = nullptr;
    };

    RenderContext* ctx_ = nullptr;
    std::unordered_map<TextureRID, TextureData> textures_;
    TextureRID next_rid_ = 1;
};

} // namespace gryce_engine::render