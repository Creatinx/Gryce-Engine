#include "components/2d/parallax_background.h"

#include "assets/asset_manager.h"
#include "assets/texture_data.h"
#include "resources/resource_path.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::components::d2::parallax {

void ParallaxBackground::draw(render::IRenderer2D* renderer) {
    if (!enabled || !renderer || layers.empty()) return;

    math::Vector2f cam = renderer->camera_center();
    math::Vector2f size = renderer->screen_size();
    if (size.x <= 0.0f || size.y <= 0.0f) return;

    // 计算视野半尺寸（考虑 zoom）
    float half_w = size.x * 0.5f / renderer->camera_zoom();
    float half_h = size.y * 0.5f / renderer->camera_zoom();

    for (auto& layer : layers) {
        if (layer.texture_path.empty()) continue;

        // 按需加载纹理
        if (!layer.texture.is_valid()) {
            auto tex_data = assets::AssetManager::instance().load<assets::TextureData>(layer.texture_path);
            if (tex_data && !tex_data->empty()) {
                layer.texture = renderer->create_texture_from_data(tex_data.get());
                layer.texture_width = tex_data->width;
                layer.texture_height = tex_data->height;
            }
        }
        if (!layer.texture.is_valid() || layer.texture_width <= 0 || layer.texture_height <= 0) continue;

        float tex_w = static_cast<float>(layer.texture_width) * layer.scale;
        float tex_h = static_cast<float>(layer.texture_height) * layer.scale;

        // 计算摄像机偏移后的层中心
        math::Vector2f layer_center = cam * layer.scroll_factor;

        // 平铺覆盖整个视野：从视野左下角开始，向上向右平铺
        float start_x = std::floor((cam.x - half_w - layer_center.x) / tex_w) * tex_w + layer_center.x;
        float start_y = std::floor((cam.y - half_h - layer_center.y) / tex_h) * tex_h + layer_center.y;

        render::ITexture* tex_ptr = renderer->resolve_texture(layer.texture);
        if (!tex_ptr) continue;

        for (float y = start_y; y < cam.y + half_h + tex_h; y += tex_h) {
            for (float x = start_x; x < cam.x + half_w + tex_w; x += tex_w) {
                renderer->draw_sprite(x, y, tex_w, tex_h, tex_ptr, layer.tint);
            }
        }
    }
}

} // namespace gryce_engine::components::d2::parallax
