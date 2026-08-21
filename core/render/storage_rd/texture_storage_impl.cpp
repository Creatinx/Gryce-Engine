#include "render/storage_rd/texture_storage_impl.h"
#include "render/render_context.h"
#include "render/texture.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::render {

TextureStorageImpl::TextureStorageImpl(RenderContext* ctx)
    : ctx_(ctx) {
}

TextureStorageImpl::~TextureStorageImpl() {
    for (auto& [rid, data] : textures_) {
        if (data.gpu_handle.is_valid()) ctx_->destroy_texture(data.gpu_handle);
    }
    textures_.clear();
}

TextureRID TextureStorageImpl::texture_create() {
    TextureRID rid = next_rid_++;
    textures_[rid] = TextureData{};
    return rid;
}

void TextureStorageImpl::texture_free(TextureRID rid) {
    auto it = textures_.find(rid);
    if (it == textures_.end()) return;
    if (it->second.gpu_handle.is_valid()) ctx_->destroy_texture(it->second.gpu_handle);
    textures_.erase(it);
}

void TextureStorageImpl::texture_set_data(TextureRID rid, TextureType type,
                                           TextureFormat format, int width, int height,
                                           int depth, int mipmaps, const void* data) {
    auto it = textures_.find(rid);
    if (it == textures_.end()) return;
    auto& td = it->second;
    td.type = type;
    td.format = format;
    td.width = width;
    td.height = height;
    td.depth = depth;
    td.mipmaps = mipmaps;
    td.pending_data = data;
    td.dirty = true;
}

void TextureStorageImpl::texture_set_path(TextureRID rid, const std::string& path) {
    auto it = textures_.find(rid);
    if (it == textures_.end()) return;
    it->second.path = path;
}

std::string TextureStorageImpl::texture_get_path(TextureRID rid) const {
    auto it = textures_.find(rid);
    return it != textures_.end() ? it->second.path : "";
}

int TextureStorageImpl::texture_get_width(TextureRID rid) const {
    auto it = textures_.find(rid);
    return it != textures_.end() ? it->second.width : 0;
}

int TextureStorageImpl::texture_get_height(TextureRID rid) const {
    auto it = textures_.find(rid);
    return it != textures_.end() ? it->second.height : 0;
}

TextureFormat TextureStorageImpl::texture_get_format(TextureRID rid) const {
    auto it = textures_.find(rid);
    return it != textures_.end() ? it->second.format : TextureFormat::RGBA8;
}

void TextureStorageImpl::update_buffers() {
    for (auto& [rid, data] : textures_) {
        if (!data.dirty || !data.pending_data) continue;

        if (data.gpu_handle.is_valid()) {
            ctx_->destroy_texture(data.gpu_handle);
        }
        data.gpu_handle = ctx_->create_texture();
        ITexture* tex = ctx_->texture(data.gpu_handle);
        if (!tex) continue;

        tex->create(data.format, data.width, data.height, data.pending_data);
        data.dirty = false;
        data.pending_data = nullptr;
    }
}

} // namespace gryce_engine::render