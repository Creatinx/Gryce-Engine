#pragma once

#include <string>

#include "components/2d/component_2d.h"
#include "render/texture.h"

namespace gryce_engine::components::d2::sprite {

// ---------------------------------------------------------------------------
// Sprite2D — 2D 精灵组件，支持 albedo 贴图与法线贴图。
// 没有贴图时使用纯色矩形。
// ---------------------------------------------------------------------------
class Sprite2D : public Component2D {
public:
    std::string texture_path;        // albedo / diffuse 贴图路径（res:/...）
    std::string normal_map_path;     // 法线贴图路径（空表示使用默认平面法线）
    render::Color color = render::Color::white();
    float width = 100.0f;
    float height = 100.0f;
    bool lit = true;                 // 是否受光照影响
    bool cast_shadow = false;        // 是否作为 2D 阴影遮挡物

    // 运行时加载的 GPU 纹理（不序列化）
    mutable render::ITexture* albedo_texture = nullptr;
    mutable render::ITexture* normal_texture = nullptr;
    mutable render::RHITextureHandle albedo_handle;
    mutable render::RHITextureHandle normal_handle;

    Sprite2D() = default;
    Sprite2D(const std::string& tex, float w, float h)
        : texture_path(tex), width(w), height(h) {}

    const char* type() const override { return "Sprite2D"; }

    void serialize(nlohmann::json& out) const override {
        Component2D::serialize_base(out);
        out["texture_path"] = texture_path;
        out["normal_map_path"] = normal_map_path;
        out["color"] = { color.r, color.g, color.b, color.a };
        out["width"] = width;
        out["height"] = height;
        out["lit"] = lit;
        out["cast_shadow"] = cast_shadow;
    }

    void deserialize(const nlohmann::json& in) override {
        Component2D::deserialize_base(in);
        texture_path = in.value("texture_path", "");
        normal_map_path = in.value("normal_map_path", "");
        auto c = in.value("color", std::vector<float>{1, 1, 1, 1});
        if (c.size() >= 4) color = render::Color(c[0], c[1], c[2], c[3]);
        width = in.value("width", 100.0f);
        height = in.value("height", 100.0f);
        lit = in.value("lit", true);
        cast_shadow = in.value("cast_shadow", false);
    }

    void draw(render::IRenderer2D* renderer) override;

    uint64_t render_hash() const override {
        uint64_t h = Component2D::render_hash();
        hash_combine(h, hash_string(texture_path));
        hash_combine(h, hash_string(normal_map_path));
        hash_combine(h, hash_float(color.r));
        hash_combine(h, hash_float(color.g));
        hash_combine(h, hash_float(color.b));
        hash_combine(h, hash_float(color.a));
        hash_combine(h, hash_float(width));
        hash_combine(h, hash_float(height));
        hash_combine(h, static_cast<uint64_t>(lit));
        hash_combine(h, static_cast<uint64_t>(cast_shadow));
        return h;
    }
};

} // namespace gryce_engine::components::d2::sprite
