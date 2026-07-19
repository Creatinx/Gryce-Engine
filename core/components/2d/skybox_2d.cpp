#include "components/2d/skybox_2d.h"

#include <mutex>
#include <unordered_map>

#include "assets/asset_manager.h"
#include "assets/texture_data.h"
#include "render/texture.h"
#include "utils/glog/glog_lib.h"

using gryce_engine::assets::AssetManager;
using gryce_engine::assets::TextureData;

namespace gryce_engine::components::d2::skybox {

namespace {

// 天空盒贴图也按路径共享 GPU 句柄
struct SkyboxTextureCache {
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

SkyboxTextureCache& skybox_texture_cache() {
    static SkyboxTextureCache cache;
    return cache;
}

} // namespace

void Skybox2D::draw(render::IRenderer2D* renderer) {
    if (!enabled || !renderer) return;

    math::Vector2f screen = renderer->screen_size();
    if (screen.x <= 0.0f || screen.y <= 0.0f) return;

    // 按需加载贴图。texture_ptr 仅用于主线程查询纹理尺寸（平铺计算），
    // 绘制一律传 texture_handle，由渲染线程执行时经 generation 校验解析。
    if (!texture_path.empty() && !texture_ptr) {
        if (!texture_handle.is_valid()) {
            texture_handle = skybox_texture_cache().get_or_create(renderer, texture_path);
        }
        if (texture_handle.is_valid()) {
            texture_ptr = renderer->resolve_texture(texture_handle);
        }
    }

    math::Vector2f cam = renderer->camera_center();
    math::Vector2f origin = cam * scroll_factor;

    if (texture_handle.is_valid() && texture_ptr && texture_ptr->is_valid()) {
        if (tile) {
            float tex_w = static_cast<float>(texture_ptr->width());
            float tex_h = static_cast<float>(texture_ptr->height());
            if (tex_w > 0.0f && tex_h > 0.0f) {
                float half_w = screen.x * 0.5f / renderer->camera_zoom();
                float half_h = screen.y * 0.5f / renderer->camera_zoom();
                float start_x = std::floor((cam.x - half_w - origin.x) / tex_w) * tex_w + origin.x;
                float start_y = std::floor((cam.y - half_h - origin.y) / tex_h) * tex_h + origin.y;
                for (float y = start_y; y < cam.y + half_h + tex_h; y += tex_h) {
                    for (float x = start_x; x < cam.x + half_w + tex_w; x += tex_w) {
                        renderer->draw_sprite(x, y, tex_w, tex_h, texture_handle, color);
                    }
                }
                return;
            }
        }
        // 拉伸单张覆盖整个视野（不受摄像机移动影响，除非 scroll_factor > 0）
        float x = origin.x - screen.x * 0.5f / renderer->camera_zoom();
        float y = origin.y - screen.y * 0.5f / renderer->camera_zoom();
        float w = screen.x / renderer->camera_zoom();
        float h = screen.y / renderer->camera_zoom();
        renderer->draw_sprite(x, y, w, h, texture_handle, color);
    } else {
        // 纯色天空背景
        float x = origin.x - screen.x * 0.5f / renderer->camera_zoom();
        float y = origin.y - screen.y * 0.5f / renderer->camera_zoom();
        float w = screen.x / renderer->camera_zoom();
        float h = screen.y / renderer->camera_zoom();
        renderer->draw_rect(x, y, w, h, color);
    }
}

} // namespace gryce_engine::components::d2::skybox
