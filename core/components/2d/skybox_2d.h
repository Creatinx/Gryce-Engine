#pragma once

#include <string>

#include "components/2d/component_2d.h"
#include "render/rhi_handle.h"

namespace gryce_engine::components::d2::skybox {

// ---------------------------------------------------------------------------
// Skybox2D — 2D 天空盒组件
// 在摄像机视野背后绘制一张全屏/平铺的天空贴图，不参与光照计算。
// - texture_path: 天空贴图路径（空则使用纯色）
// - color: 无贴图时的纯色，或有贴图时的叠加色调
// - scroll_factor: 0 = 完全跟随摄像机（默认天空盒），1 = 与摄像机同步移动
// ---------------------------------------------------------------------------
class Skybox2D : public Component2D {
public:
    std::string texture_path;
    render::Color color = render::Color(0.05f, 0.05f, 0.10f, 1.0f);
    float scroll_factor = 0.0f;   // 0=固定背景，1=跟随摄像机
    bool tile = false;            // true=平铺覆盖视野，false=拉伸单张覆盖视野

    // 运行时 GPU 纹理（不序列化）。绘制只传 texture_handle；
    // texture_ptr 仅用于主线程查询纹理尺寸（平铺计算），不捕获进渲染命令。
    mutable render::RHITextureHandle texture_handle;
    mutable render::ITexture* texture_ptr = nullptr;

    Skybox2D() = default;
    explicit Skybox2D(const std::string& path)
        : texture_path(path) {}

    const char* type() const override { return "Skybox2D"; }

    void serialize(nlohmann::json& out) const override {
        Component2D::serialize_base(out);
        out["texture_path"] = texture_path;
        out["color"] = { color.r, color.g, color.b, color.a };
        out["scroll_factor"] = scroll_factor;
        out["tile"] = tile;
    }

    void deserialize(const nlohmann::json& in) override {
        Component2D::deserialize_base(in);
        texture_path = in.value("texture_path", "");
        auto c = in.value("color", std::vector<float>{0.05f, 0.05f, 0.10f, 1.0f});
        if (c.size() >= 4) color = render::Color(c[0], c[1], c[2], c[3]);
        scroll_factor = in.value("scroll_factor", 0.0f);
        tile = in.value("tile", false);
    }

    void draw(render::IRenderer2D* renderer) override;

    uint64_t render_hash() const override {
        uint64_t h = Component2D::render_hash();
        hash_combine(h, hash_string(texture_path));
        hash_combine(h, hash_float(color.r));
        hash_combine(h, hash_float(color.g));
        hash_combine(h, hash_float(color.b));
        hash_combine(h, hash_float(color.a));
        hash_combine(h, hash_float(scroll_factor));
        hash_combine(h, static_cast<uint64_t>(tile));
        return h;
    }
};

} // namespace gryce_engine::components::d2::skybox
