#pragma once

#include <string>
#include <vector>

#include "components/2d/component_2d.h"
#include "render/render2d.h"
#include "render/rhi_handle.h"

namespace gryce_engine::components::d2::parallax {

// ---------------------------------------------------------------------------
// ParallaxLayer — 单张视差滚动层
// ---------------------------------------------------------------------------
struct ParallaxLayer {
    std::string texture_path; // res:/textures/...
    float scroll_factor = 0.5f; // 相对于摄像机的移动比例（0=不移动，1=同步移动）
    float scale = 1.0f;         // 纹理绘制缩放
    render::Color tint = render::Color::white();

    // 运行时纹理句柄（不序列化）
    mutable render::RHITextureHandle texture;
    mutable int texture_width = 0;
    mutable int texture_height = 0;

    void serialize(nlohmann::json& out) const {
        out["texture_path"] = texture_path;
        out["scroll_factor"] = scroll_factor;
        out["scale"] = scale;
        out["tint"] = { tint.r, tint.g, tint.b, tint.a };
    }

    void deserialize(const nlohmann::json& in) {
        texture_path = in.value("texture_path", "");
        scroll_factor = in.value("scroll_factor", 0.5f);
        scale = in.value("scale", 1.0f);
        auto c = in.value("tint", std::vector<float>{1, 1, 1, 1});
        if (c.size() >= 4) tint = render::Color(c[0], c[1], c[2], c[3]);
    }
};

// ---------------------------------------------------------------------------
// ParallaxBackground — 多层视差滚动背景。
// 绘制时根据当前 Camera2D 中心计算每层偏移，产生深度感。
// ---------------------------------------------------------------------------
class ParallaxBackground : public Component2D {
public:
    std::vector<ParallaxLayer> layers;

    ParallaxBackground() { render_order = -1000; }

    const char* type() const override { return "ParallaxBackground"; }

    void serialize(nlohmann::json& out) const override {
        Component2D::serialize_base(out);
        out["layers"] = nlohmann::json::array();
        for (const auto& layer : layers) {
            nlohmann::json j;
            layer.serialize(j);
            out["layers"].push_back(j);
        }
    }

    void deserialize(const nlohmann::json& in) override {
        Component2D::deserialize_base(in);
        layers.clear();
        if (in.contains("layers") && in["layers"].is_array()) {
            for (const auto& j : in["layers"]) {
                ParallaxLayer layer;
                layer.deserialize(j);
                layers.push_back(layer);
            }
        }
    }

    void draw(render::IRenderer2D* renderer) override;

    uint64_t render_hash() const override {
        uint64_t h = Component2D::render_hash();
        hash_combine(h, static_cast<uint64_t>(layers.size()));
        for (const auto& layer : layers) {
            hash_combine(h, hash_string(layer.texture_path));
            hash_combine(h, hash_float(layer.scroll_factor));
            hash_combine(h, hash_float(layer.scale));
            hash_combine(h, hash_float(layer.tint.r));
            hash_combine(h, hash_float(layer.tint.g));
            hash_combine(h, hash_float(layer.tint.b));
            hash_combine(h, hash_float(layer.tint.a));
        }
        return h;
    }
};

} // namespace gryce_engine::components::d2::parallax
