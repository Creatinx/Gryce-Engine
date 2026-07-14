#include "components/2d/tilemap.h"

#include <fstream>

#include "render/render2d.h"
#include "assets/asset_manager.h"
#include "resources/resource_path.h"
#include "scene/entity.h"
#include "components/static_body_2d.h"
#include "components/box_collider_2d.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::components::d2::tilemap {

namespace {

// 简单的哈希彩色生成器
float frac(float x) { return x - std::floor(x); }

} // namespace

render::Color Tilemap::tile_color(int index) {
    if (index < 0) return render::Color::white();
    // 使用黄金角分布的色相，保证相邻索引颜色差异明显
    float hue = frac(static_cast<float>(index) * 0.61803398875f);
    float r = frac(hue * 6.0f);
    float g = frac(hue * 6.0f + 2.0f);
    float b = frac(hue * 6.0f + 4.0f);
    // 增强对比度
    r = r > 0.5f ? 0.7f + 0.3f * (r - 0.5f) * 2.0f : 0.3f + 0.4f * r * 2.0f;
    g = g > 0.5f ? 0.7f + 0.3f * (g - 0.5f) * 2.0f : 0.3f + 0.4f * g * 2.0f;
    b = b > 0.5f ? 0.7f + 0.3f * (b - 0.5f) * 2.0f : 0.3f + 0.4f * b * 2.0f;
    return render::Color(r, g, b, 1.0f);
}

void Tilemap::ensure_tileset_loaded(render::IRenderer2D* renderer) const {
    // 1) 按需解析 JSON（物理属性等）
    if (!tileset_json_loaded_ && !tileset_path.empty()) {
        tileset_json_loaded_ = true;

        std::string resolved = resources::ResourcePath::resolve(tileset_path);
        std::ifstream file(resolved);
        if (file.is_open()) {
            try {
                nlohmann::json j;
                file >> j;
                tileset.deserialize(j);
            } catch (const std::exception& e) {
                GLOG_WARN("Tilemap: failed to parse tileset '{}': {}", tileset_path, e.what());
            }
        } else {
            GLOG_WARN("Tilemap: failed to open tileset '{}'", tileset_path);
        }
    }

    // 2) 按需上传 GPU 纹理
    if (tileset_texture_loaded_ || tileset.texture_path.empty()) return;
    tileset_texture_loaded_ = true;

    auto tex_data = assets::AssetManager::instance().load<assets::TextureData>(tileset.texture_path);
    if (!tex_data || tex_data->empty()) {
        GLOG_WARN("Tilemap: failed to load texture data '{}'", tileset.texture_path);
        return;
    }

    tileset_texture_width_ = tex_data->width;
    tileset_texture_height_ = tex_data->height;

    // 如果 JSON 没有给出 tile_count，根据纹理尺寸和瓦片尺寸推算
    if (tileset.tile_count <= 0 && tileset.tile_width > 0 && tileset.tile_height > 0) {
        int usable_w = tileset_texture_width_ - 2 * tileset.margin;
        int usable_h = tileset_texture_height_ - 2 * tileset.margin;
        int cols = (usable_w + tileset.spacing) / (tileset.tile_width + tileset.spacing);
        int rows = (usable_h + tileset.spacing) / (tileset.tile_height + tileset.spacing);
        if (cols > 0 && rows > 0) {
            tileset.tile_count = cols * rows;
        }
        GLOG_INFO("Tilemap: computed tile_count={} cols={} rows={} from {}x{} tile={}",
                  tileset.tile_count, cols, rows, tileset_texture_width_, tileset_texture_height_, tileset.tile_width);
    }

    if (use_tileset_texture && renderer) {
        tileset.texture = renderer->create_texture_from_data(tex_data.get());
        if (tileset.texture.is_valid()) {
            GLOG_INFO("Tilemap: loaded tileset texture '{}' ({}x{}, {} channels)",
                      tileset.texture_path, tex_data->width, tex_data->height,
                      tex_data->channels);
        }
    }
}

void Tilemap::draw(render::IRenderer2D* renderer) {
    if (!enabled || !renderer || map_width <= 0 || map_height <= 0) return;

    ensure_tileset_loaded(renderer);
    bool use_texture = use_tileset_texture && tileset.texture.is_valid() &&
                       tileset_texture_width_ > 0 && tileset_texture_height_ > 0;

    math::Vector2f pos = position();
    math::Vector2f s = scale();
    float cw = cell_width * s.x;
    float ch = cell_height * s.y;

    for (int y = 0; y < map_height; ++y) {
        for (int x = 0; x < map_width; ++x) {
            int tile = get_tile(x, y);
            if (tile < 0) continue;

            float wx = pos.x + static_cast<float>(x) * cw;
            float wy = pos.y + static_cast<float>(y) * ch;

            if (use_texture) {
                math::Vector4f uv = tileset.tile_uv(tile, tileset_texture_width_, tileset_texture_height_);
                render::ITexture* tex_ptr = renderer->resolve_texture(tileset.texture);
                if (!tex_ptr) {
                    renderer->draw_rect(wx, wy, cw, ch, tile_color(tile));
                    continue;
                }
                if (lit) {
                    renderer->draw_lit_sprite_region(wx, wy, cw, ch,
                                                     uv.x, uv.y, uv.z, uv.w,
                                                     tex_ptr, nullptr, render::Color::white());
                } else {
                    renderer->draw_sprite_region(wx, wy, cw, ch,
                                                  uv.x, uv.y, uv.z, uv.w,
                                                  tex_ptr, render::Color::white());
                }
            } else {
                renderer->draw_rect(wx, wy, cw, ch, tile_color(tile));
            }

            if (cast_shadow) {
                renderer->draw_shadow_caster(wx, wy, cw, ch);
            }

            if (debug_draw_colliders && generate_colliders) {
                renderer->draw_rect(wx + cw * 0.45f, wy + ch * 0.45f,
                                    cw * 0.1f, ch * 0.1f, render::Color::red());
            }
        }
    }
}

namespace {

bool load_tileset_json(const std::string& path, resources::Tileset& out) {
    if (path.empty()) return false;
    std::string resolved = resources::ResourcePath::resolve(path);
    if (resolved.empty()) return false;
    std::ifstream file(resolved);
    if (!file.is_open()) return false;
    try {
        nlohmann::json j;
        file >> j;
        out.deserialize(j);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace

bool Tilemap::is_solid_tile(int x, int y) const {
    int tile = get_tile(x, y);
    if (tile < 0) return false;
    if (!tileset_json_loaded_ && !tileset_path.empty()) {
        tileset_json_loaded_ = load_tileset_json(tileset_path, tileset);
    }
    // 如果图集没有定义属性，兼容旧行为：所有非空瓦片都视为 solid
    if (tileset.tile_properties.empty()) return true;
    return tileset.is_solid(tile);
}

bool Tilemap::is_outer_ring_tile(int x, int y) const {
    if (!is_solid_tile(x, y)) return false;
    // 地图边缘即外圈
    if (x == 0 || x == map_width - 1 || y == 0 || y == map_height - 1) return true;
    // 任一四邻域非 solid 即为外圈
    return !is_solid_tile(x - 1, y) || !is_solid_tile(x + 1, y) ||
           !is_solid_tile(x, y - 1) || !is_solid_tile(x, y + 1);
}

void Tilemap::on_init() {
    if (!generate_colliders || !owner() || map_width <= 0 || map_height <= 0) return;

    // 确保 tileset 属性已加载，以便正确判断 solid/outer-ring
    if (!tileset_json_loaded_ && !tileset_path.empty()) {
        tileset_json_loaded_ = load_tileset_json(tileset_path, tileset);
    }

    math::Vector2f pos = position();
    math::Vector2f s = scale();
    float cw = cell_width * s.x;
    float ch = cell_height * s.y;

    // 只对外圈 solid 瓦片按行水平合并生成碰撞体
    for (int y = 0; y < map_height; ++y) {
        int x = 0;
        while (x < map_width) {
            while (x < map_width && !is_outer_ring_tile(x, y)) ++x;
            if (x >= map_width) break;
            int start = x;
            while (x < map_width && is_outer_ring_tile(x, y)) ++x;
            int end = x; // [start, end)

            int count = end - start;
            float wx = pos.x + (start + count * 0.5f) * cw;
            float wy = pos.y + (y + 0.5f) * ch;

            std::string col_name = "TileCollider_" + std::to_string(y) + "_" + std::to_string(start);
            auto child = std::make_unique<scene::Entity>(col_name);
            child->transform()->position = math::Vector3f(wx, wy, 0.0f);
            child->add_component<components::StaticBody2D>();
            auto* col = child->add_component<components::BoxCollider2D>();
            col->size = math::Vector2f(count * cw, ch);
            owner()->add_child(std::move(child));

            GLOG_INFO("Tilemap: generated outer-ring collider '{}' ({}x{} tiles)", col_name, count, 1);
        }
    }
}

} // namespace gryce_engine::components::d2::tilemap
