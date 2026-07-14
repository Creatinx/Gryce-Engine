#pragma once

#include <vector>
#include <string>

#include "components/2d/component_2d.h"
#include "resources/tileset.h"
#include "assets/texture_data.h"

namespace gryce_engine::scene {
class Entity;
} // namespace gryce_engine::scene

namespace gryce_engine::components::d2::tilemap {

// ---------------------------------------------------------------------------
// Tilemap — 2D 瓦片地图组件。
// 以网格方式组织瓦片索引，使用关联的 Tileset 资源渲染。
// 当前渲染后端若不支持图集 UV 裁剪，则使用索引对应颜色绘制纯色方块。
// ---------------------------------------------------------------------------
class Tilemap : public Component2D {
public:
    std::string tileset_path;   // Tileset 资源路径（res:/tilesets/xxx.json）

    int map_width = 0;          // 地图宽度（格子数）
    int map_height = 0;         // 地图高度（格子数）
    float cell_width = 32.0f;   // 单个格子世界宽度
    float cell_height = 32.0f;  // 单个格子世界高度

    // 瓦片数据，一维数组，索引 = y * map_width + x；
    // -1 表示空格子，>=0 表示 Tileset 中瓦片索引。
    std::vector<int> tiles;

    // 是否为每个非空瓦片生成碰撞体（StaticBody2D + BoxCollider2D）。
    // 开启后会在 on_init / 反序列化时自动创建子实体作为碰撞体。
    bool generate_colliders = false;

    // 是否可见碰撞体占位（调试用）
    bool debug_draw_colliders = false;

    // 是否使用 Tileset 纹理渲染（false 时使用索引彩色方块）
    bool use_tileset_texture = true;

    // 是否受 2D 光照影响（使用延迟光照渲染）
    bool lit = false;

    // 是否作为 2D 阴影遮挡物
    bool cast_shadow = false;

    // 运行时缓存的 Tileset
    mutable resources::Tileset tileset;

    // 运行时缓存的图集 CPU 像素尺寸（用于 UV 归一化）
    mutable int tileset_texture_width_ = 0;
    mutable int tileset_texture_height_ = 0;

    Tilemap() = default;
    Tilemap(int w, int h, float cw, float ch)
        : map_width(w), map_height(h), cell_width(cw), cell_height(ch) {
        tiles.resize(static_cast<size_t>(w) * static_cast<size_t>(h), -1);
    }

    void resize(int w, int h) {
        map_width = w;
        map_height = h;
        tiles.assign(static_cast<size_t>(w) * static_cast<size_t>(h), -1);
    }

    void set_tile(int x, int y, int tile_index) {
        if (x < 0 || x >= map_width || y < 0 || y >= map_height) return;
        size_t index = static_cast<size_t>(y) * static_cast<size_t>(map_width) + static_cast<size_t>(x);
        tiles[index] = tile_index;
    }

    int get_tile(int x, int y) const {
        if (map_width <= 0 || map_height <= 0 || x < 0 || x >= map_width || y < 0 || y >= map_height) return -1;
        size_t index = static_cast<size_t>(y) * static_cast<size_t>(map_width) + static_cast<size_t>(x);
        return tiles[index];
    }

    const char* type() const override { return "Tilemap"; }

    void serialize(nlohmann::json& out) const override {
        Component2D::serialize_base(out);
        out["tileset_path"] = tileset_path;
        out["map_width"] = map_width;
        out["map_height"] = map_height;
        out["cell_width"] = cell_width;
        out["cell_height"] = cell_height;
        out["generate_colliders"] = generate_colliders;
        out["debug_draw_colliders"] = debug_draw_colliders;
        out["use_tileset_texture"] = use_tileset_texture;
        out["lit"] = lit;
        out["cast_shadow"] = cast_shadow;
        out["tiles"] = tiles;
    }

    void deserialize(const nlohmann::json& in) override {
        Component2D::deserialize_base(in);
        tileset_path = in.value("tileset_path", "");
        map_width = in.value("map_width", 0);
        map_height = in.value("map_height", 0);
        cell_width = in.value("cell_width", 32.0f);
        cell_height = in.value("cell_height", 32.0f);
        generate_colliders = in.value("generate_colliders", false);
        debug_draw_colliders = in.value("debug_draw_colliders", false);
        use_tileset_texture = in.value("use_tileset_texture", true);
        lit = in.value("lit", false);
        cast_shadow = in.value("cast_shadow", false);
        tiles = in.value("tiles", std::vector<int>());
        const size_t expected = static_cast<size_t>(map_width) * static_cast<size_t>(map_height);
        if (tiles.size() != expected) {
            tiles.resize(expected, -1);
        }
    }

    void draw(render::IRenderer2D* renderer) override;
    void on_init() override;

    // 判断 (x,y) 处瓦片是否为 solid（会触发 tileset 加载）
    bool is_solid_tile(int x, int y) const;

    // 判断 (x,y) 处瓦片是否属于 solid 区域的最外围一圈
    bool is_outer_ring_tile(int x, int y) const;

    uint64_t render_hash() const override {
        uint64_t h = Component2D::render_hash();
        hash_combine(h, hash_string(tileset_path));
        hash_combine(h, static_cast<uint64_t>(map_width));
        hash_combine(h, static_cast<uint64_t>(map_height));
        hash_combine(h, hash_float(cell_width));
        hash_combine(h, hash_float(cell_height));
        hash_combine(h, static_cast<uint64_t>(use_tileset_texture));
        hash_combine(h, static_cast<uint64_t>(lit));
        hash_combine(h, static_cast<uint64_t>(cast_shadow));
        // 瓦片内容通常不变，完整哈希保证修改后能检测到
        for (int v : tiles) {
            hash_combine(h, static_cast<uint64_t>(static_cast<uint32_t>(v) + 1));
        }
        return h;
    }

private:
    // 根据瓦片索引生成一个稳定的伪彩色（用于无贴图时区分瓦片）
    static render::Color tile_color(int index);

    // 按需加载 Tileset JSON 与纹理
    void ensure_tileset_loaded(render::IRenderer2D* renderer) const;

    mutable bool tileset_json_loaded_ = false;     // JSON 属性是否已解析
    mutable bool tileset_texture_loaded_ = false;  // GPU 纹理是否已上传
};

} // namespace gryce_engine::components::d2::tilemap
