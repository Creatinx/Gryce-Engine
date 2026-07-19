#include "components/2d/sprite_2d.h"

#include <mutex>
#include <unordered_map>

#include "render/render_context.h"
#include "assets/asset_manager.h"
#include "assets/texture_data.h"
#include "utils/glog/glog_lib.h"

using gryce_engine::assets::AssetManager;
using gryce_engine::assets::TextureData;

namespace gryce_engine::components::d2::sprite {

namespace {

// 简单按路径共享 GPU 纹理，避免同一张贴图被重复上传（如 24 个金币生成 24 张 GPU 纹理）。
// 注意：当前缓存跨 renderer 会话不自动失效；引擎示例通常只使用一个 renderer 会话。
struct SpriteTextureCache {
    std::mutex mutex;
    std::unordered_map<std::string, render::RHITextureHandle> handles;

    render::RHITextureHandle get_or_create(render::IRenderer2D* renderer, const std::string& path) {
        if (path.empty() || !renderer) return render::RHITextureHandle{};
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto it = handles.find(path);
            if (it != handles.end() && it->second.is_valid()) {
                return it->second;
            }
        }
        auto data = AssetManager::instance().load<TextureData>(path);
        if (!data) return render::RHITextureHandle{};
        render::RHITextureHandle handle = renderer->create_texture_from_data(data.get());
        if (handle.is_valid()) {
            std::lock_guard<std::mutex> lock(mutex);
            handles[path] = handle;
        }
        return handle;
    }
};

SpriteTextureCache& sprite_texture_cache() {
    static SpriteTextureCache cache;
    return cache;
}

} // namespace

void Sprite2D::draw(render::IRenderer2D* renderer) {
    if (!enabled || !renderer) return;

    math::Vector2f pos = position();
    math::Vector2f s = scale();
    float sw = width * s.x;
    float sh = height * s.y;

    // 按需加载贴图：CPU 数据由 AssetManager 缓存，GPU 纹理按路径共享。
    // 绘制只传句柄，不再解析/缓存裸指针。
    if (!texture_path.empty() && !albedo_handle.is_valid()) {
        albedo_handle = sprite_texture_cache().get_or_create(renderer, texture_path);
    }
    if (!normal_map_path.empty() && !normal_handle.is_valid()) {
        normal_handle = sprite_texture_cache().get_or_create(renderer, normal_map_path);
    }

    if (lit) {
        renderer->draw_lit_sprite(pos.x - sw * 0.5f, pos.y - sh * 0.5f,
                                   sw, sh, albedo_handle, normal_handle, color);
    } else {
        renderer->draw_sprite(pos.x - sw * 0.5f, pos.y - sh * 0.5f,
                              sw, sh, albedo_handle, color);
    }

    if (cast_shadow) {
        renderer->draw_shadow_caster(pos.x - sw * 0.5f, pos.y - sh * 0.5f, sw, sh);
    }
}

} // namespace gryce_engine::components::d2::sprite
