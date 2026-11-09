#include "ui/ui_texture.h"

#include <mutex>
#include <unordered_map>

namespace gryce_engine::ui {

namespace {

struct TextureStore {
    std::mutex mutex;
    UITextureLoadFn loader = nullptr;
    std::unordered_map<std::string, TextureRef> cache;
};

TextureStore& store() {
    static TextureStore instance;
    return instance;
}

} // namespace

void ui_set_texture_loader(UITextureLoadFn fn) {
    TextureStore& s = store();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.loader != fn) s.cache.clear();
    s.loader = fn;
}

UITextureLoadFn ui_texture_loader() {
    TextureStore& s = store();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.loader;
}

TextureRef ui_texture(const std::string& path) {
    if (path.empty()) return TextureRef{};
    TextureStore& s = store();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.loader) return TextureRef{};
    const auto it = s.cache.find(path);
    if (it != s.cache.end()) return it->second;
    TextureRef ref;
    if (!s.loader(path.c_str(), &ref) || !ref.valid()) {
        // 失败也缓存一个无效项，避免每帧对同一张不存在的图重复尝试解码。
        s.cache.emplace(path, TextureRef{});
        return TextureRef{};
    }
    s.cache.emplace(path, ref);
    return ref;
}

TextureRef ui_texture_cached(const std::string& path) {
    if (path.empty()) return TextureRef{};
    TextureStore& s = store();
    std::lock_guard<std::mutex> lock(s.mutex);
    const auto it = s.cache.find(path);
    return it != s.cache.end() ? it->second : TextureRef{};
}

void ui_clear_texture_cache() {
    TextureStore& s = store();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.cache.clear();
}

} // namespace gryce_engine::ui
