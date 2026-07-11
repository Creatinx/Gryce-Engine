#include "components/2d/sprite_2d.h"

#include "render/render_context.h"
#include "assets/asset_manager.h"
#include "assets/texture_data.h"
#include "utils/glog/glog_lib.h"

using gryce_engine::assets::AssetManager;
using gryce_engine::assets::TextureData;

namespace gryce_engine::components::d2::sprite {

void Sprite2D::draw(render::IRenderer2D* renderer) {
    if (!enabled || !renderer) return;

    math::Vector2f pos = position();
    math::Vector2f s = scale();
    float sw = width * s.x;
    float sh = height * s.y;

    // 按需加载贴图（CPU 侧缓存，GPU 上传由渲染器按需完成）
    if (!texture_path.empty() && !albedo_texture) {
        auto handle = AssetManager::instance().load<TextureData>(texture_path);
        if (handle) {
            // TODO: 当 RHI 支持从 TextureData 创建纹理后，这里上传 GPU
            // albedo_texture = ...
        }
    }
    if (!normal_map_path.empty() && !normal_texture) {
        auto handle = AssetManager::instance().load<TextureData>(normal_map_path);
        if (handle) {
            // TODO: 上传法线贴图到 GPU
            // normal_texture = ...
        }
    }

    if (lit) {
        renderer->draw_lit_sprite(pos.x - sw * 0.5f, pos.y - sh * 0.5f,
                                   sw, sh, albedo_texture, normal_texture, color);
    } else {
        renderer->draw_sprite(pos.x - sw * 0.5f, pos.y - sh * 0.5f,
                              sw, sh, albedo_texture, color);
    }
}

} // namespace gryce_engine::components::d2::sprite
