#pragma once

#include <string>
#include <vector>

#include "math/math.h"
#include "render/rhi_handle.h"

#ifdef GRYCE_HAS_NLOHMANN_JSON
#include <nlohmann/json.hpp>
#endif

namespace gryce_engine::render {
class ITexture;
} // namespace gryce_engine::render

namespace gryce_engine::resources {

// ---------------------------------------------------------------------------
// Tileset — 2D 瓦片图集资源定义。
// 描述一张瓦片图集如何被切分为若干等大的瓦片。
// ---------------------------------------------------------------------------
class Tileset {
public:
    std::string texture_path;   // 图集贴图路径（res:/textures/...）
    std::string name;           // 瓦片集名称

    int tile_width = 32;        // 单个瓦片像素宽
    int tile_height = 32;       // 单个瓦片像素高
    int columns = 1;            // 图集中每行瓦片数
    int margin = 0;             // 图集外边框像素
    int spacing = 0;            // 瓦片之间间隔像素
    int tile_count = 1;         // 总瓦片数

    // 运行时加载的 GPU 纹理句柄（不序列化）
    mutable render::RHITextureHandle texture;

    Tileset() = default;

    // 计算瓦片索引对应的 UV 归一化坐标（左上角原点）。
    // 返回 {u0, v0, u1, v1}
    math::Vector4f tile_uv(int tile_index, int tex_w, int tex_h) const {
        if (columns <= 0 || tile_count <= 0 || tile_index < 0 || tile_index >= tile_count ||
            tex_w <= 0 || tex_h <= 0) {
            return math::Vector4f(0.0f, 0.0f, 1.0f, 1.0f);
        }
        int col = tile_index % columns;
        int row = tile_index / columns;

        float x0 = static_cast<float>(margin + col * (tile_width + spacing));
        float y0 = static_cast<float>(margin + row * (tile_height + spacing));
        float x1 = x0 + static_cast<float>(tile_width);
        float y1 = y0 + static_cast<float>(tile_height);

        return math::Vector4f(
            x0 / static_cast<float>(tex_w),
            y0 / static_cast<float>(tex_h),
            x1 / static_cast<float>(tex_w),
            y1 / static_cast<float>(tex_h)
        );
    }

#ifdef GRYCE_HAS_NLOHMANN_JSON
    void serialize(nlohmann::json& out) const {
        out["texture_path"] = texture_path;
        out["name"] = name;
        out["tile_width"] = tile_width;
        out["tile_height"] = tile_height;
        out["columns"] = columns;
        out["margin"] = margin;
        out["spacing"] = spacing;
        out["tile_count"] = tile_count;
    }

    void deserialize(const nlohmann::json& in) {
        texture_path = in.value("texture_path", "");
        name = in.value("name", "");
        tile_width = in.value("tile_width", 32);
        tile_height = in.value("tile_height", 32);
        columns = in.value("columns", 1);
        margin = in.value("margin", 0);
        spacing = in.value("spacing", 0);
        tile_count = in.value("tile_count", 1);
    }
#endif
};

} // namespace gryce_engine::resources
