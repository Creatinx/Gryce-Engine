#pragma once

#include "export.h"
#include "components/component.h"
#include "math/math.h"
#include "render/render2d.h"

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// FogVolume — 体积雾区域（纯数据组件）
//
// 与 Skybox3D 同类：不新增着色器，也不持有 GPU 资源，而是由
// RenderPipeline::collect_fog_volumes 把组件参数映射到引擎既有的全局体积雾
// （VolumetricFog_RD，经 fog_ / set_fog_enabled / set_fog_params 暴露）。
//
// 字段语义：
//   - color：雾颜色（雾的散射色，逐体积归并后取主导体积的颜色）。
//   - density：雾密度；实际写入的值还会乘上"尺寸因子"（见下）。
//   - height_falloff：映射到全局雾的高度尺度 uFogHeight。其含义是雾密度的
//     指数高度衰减尺度 exp(-|y|/height_falloff)：值越大，雾在竖直方向铺得越高、
//     越不容易在离地后变淡（因此 height_falloff 真实影响画面）。
//   - size：体积尺寸。引擎的体积雾是全局单一状态，没有逐体积的局部边界，因此
//     size 通过尺寸因子 cbrt(size.x*size.y*size.z)/5 折算进有效密度：默认尺寸
//     (5,5,5) 对应因子 1.0，体积越大雾越浓。这使 size 真实参与结果。
//   - volumetric：true = 参与体积雾；false = 视为"不参与体积雾"，映射到最省的
//     路径——不开启体积雾 pass（该体积贡献零雾）。
//
// 能力缺口（诚实说明）：全局体积雾只有一组 color/density/height，无法表达
// "逐体积的局部雾"；多个体积被归并为一个全局近似（见 collect_fog_volumes 的
// 归并规则）。体积的世界位置不参与归并，当前不生效。
// ---------------------------------------------------------------------------
class GRYCE_API FogVolume : public Component {
public:
    render::Color color = render::Color(0.7f, 0.8f, 0.9f, 1.0f);
    float density = 0.05f;
    float height_falloff = 1.0f;
    math::Vector3f size = math::Vector3f(5.0f, 5.0f, 5.0f);
    bool volumetric = true;

    FogVolume() = default;

    const char* type() const override { return "FogVolume"; }

    void serialize(nlohmann::json& out) const override {
        out["color"] = { color.r, color.g, color.b, color.a };
        out["density"] = density;
        out["height_falloff"] = height_falloff;
        out["size"] = { size.x, size.y, size.z };
        out["volumetric"] = volumetric;
    }
    void deserialize(const nlohmann::json& in) override {
        auto c = in.value("color", std::vector<float>{0.7f, 0.8f, 0.9f, 1.0f});
        if (c.size() >= 4) color = render::Color(c[0], c[1], c[2], c[3]);
        density = in.value("density", 0.05f);
        height_falloff = in.value("height_falloff", 1.0f);
        auto s = in.value("size", std::vector<float>{5, 5, 5});
        if (s.size() >= 3) size = math::Vector3f(s[0], s[1], s[2]);
        volumetric = in.value("volumetric", true);
    }
};

} // namespace gryce_engine::components