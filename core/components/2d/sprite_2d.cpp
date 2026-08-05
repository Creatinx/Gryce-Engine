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

// std::pair 没有 std::hash 特化，提供自定义哈希作为 unordered_map 键
struct TextureCacheKeyHash {
    std::size_t operator()(const std::pair<const void*, std::string>& key) const {
        std::size_t h1 = std::hash<const void*>{}(key.first);
        std::size_t h2 = std::hash<std::string>{}(key.second);
        return h1 ^ (h2 << 1);
    }
};

// 简单按路径共享 GPU 纹理，避免同一张贴图被重复上传（如 24 个金币生成 24 张 GPU 纹理）。
// 以 (renderer, path) 为键：不同渲染会话（Viewport / Game / 管线重建）各自持有独立的
// GPU 纹理，避免跨上下文共享句柄；renderer 会话重建后旧句柄经 generation 校验失效，
// 会自动重新上传。
struct SpriteTextureCache {
    std::mutex mutex;
    std::unordered_map<std::pair<const void*, std::string>, render::RHITextureHandle, TextureCacheKeyHash> handles;

    render::RHITextureHandle get_or_create(render::IRenderer2D* renderer, const std::string& path) {
        if (path.empty() || !renderer) return render::RHITextureHandle{};
        // 全程持锁：避免两个线程同时 miss 后重复创建（重复上传 + 泄漏一张 GPU 纹理）
        std::lock_guard<std::mutex> lock(mutex);
        const auto key = std::make_pair(static_cast<const void*>(renderer), path);
        auto it = handles.find(key);
        if (it != handles.end()) {
            // 句柄在当前 renderer 中仍有效（generation 校验）则直接复用
            if (it->second.is_valid() && renderer->resolve_texture(it->second) != nullptr) {
                return it->second;
            }
            handles.erase(it);
        }
        auto data = AssetManager::instance().load<TextureData>(path);
        if (!data) return render::RHITextureHandle{};
        render::RHITextureHandle handle = renderer->create_texture_from_data(data.get());
        if (handle.is_valid()) {
            handles[key] = handle;
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
