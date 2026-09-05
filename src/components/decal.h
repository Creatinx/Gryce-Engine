#pragma once

#include "components/component.h"
#include "math/math.h"

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// Decal — 贴花组件
// 定义贴花的位置、大小、颜色、混合参数以及纹理索引。
// 贴花渲染器在 Forward Clustered 管线的不透明 pass 之后、天空盒之前，
// 通过深度缓冲重建世界坐标，将贴花纹理投影到场景几何体表面。
// ---------------------------------------------------------------------------
class Decal : public Component {
public:
    bool enabled = true;
    math::Vector3f position;
    math::Vector3f size = math::Vector3f(1.0f, 1.0f, 1.0f);
    math::Vector3f color = math::Vector3f(1.0f, 1.0f, 1.0f);
    float albedo_blend = 1.0f;
    float normal_blend = 1.0f;
    float roughness = 0.5f;
    float metallic = 0.0f;
    float ao = 1.0f;
    int texture_id = -1; // 贴花纹理索引（-1 表示不使用纹理，仅使用颜色）

    Decal() = default;

    const char* type() const override { return "Decal"; }

    void serialize(nlohmann::json& out) const override {
        out["enabled"] = enabled;
        out["position"] = { position.x, position.y, position.z };
        out["size"] = { size.x, size.y, size.z };
        out["color"] = { color.x, color.y, color.z };
        out["albedo_blend"] = albedo_blend;
        out["normal_blend"] = normal_blend;
        out["roughness"] = roughness;
        out["metallic"] = metallic;
        out["ao"] = ao;
        out["texture_id"] = texture_id;
    }

    void deserialize(const nlohmann::json& in) override {
        enabled = in.value("enabled", true);
        auto pos = in.value("position", std::vector<float>{0, 0, 0});
        if (pos.size() >= 3) position = math::Vector3f(pos[0], pos[1], pos[2]);
        auto sz = in.value("size", std::vector<float>{1, 1, 1});
        if (sz.size() >= 3) size = math::Vector3f(sz[0], sz[1], sz[2]);
        auto col = in.value("color", std::vector<float>{1, 1, 1});
        if (col.size() >= 3) color = math::Vector3f(col[0], col[1], col[2]);
        albedo_blend = in.value("albedo_blend", 1.0f);
        normal_blend = in.value("normal_blend", 1.0f);
        roughness = in.value("roughness", 0.5f);
        metallic = in.value("metallic", 0.0f);
        ao = in.value("ao", 1.0f);
        texture_id = in.value("texture_id", -1);
    }
};

} // namespace gryce_engine::components