#pragma once

#include "export.h"
#include "components/component.h"
#include "math/math.h"

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// LightProbeGroup — 光照探针组（动态物体间接光）
//
// 纯数据组件（与 Skybox3D 同类）：不持有 GPU 资源、不新增着色器。由
// RenderPipeline::collect_light_probes 消费：从管线已持有的环境 IBL irradiance
// （CPU 侧的 irradiance 六面数据，由 upload_ibl_data 缓存）按网格方向数采样出
// 一组低频间接辐照度，乘以 intensity，再按探针体（size）与相机的空间关系做
// 衰减，最后并入 uAmbient（PBR 着色器里的间接漫反射项）。
//
// 诚实说明（能力边界）：
//  - 引擎的 PBR 着色器没有"按位置查探针音量"的接口：间接漫反射只有 uAmbient
//    这一项平坦颜色 + 全局 IBL 贴图。因此本组件采用"从管线已有环境 IBL 推导
//    辐照度颜色，再并入 uAmbient"的做法——这比简单地把 ambient 乘 intensity
//    更忠实（颜色来自真实环境），但仍不是逐探针插值。
//  - grid_x/y/z 决定环境采样方向数（即估计辐照度的精度），不是逐格点存辐照度。
//  - size 决定探针体半尺寸，用于按相机与探针体的距离做贡献衰减。
// ---------------------------------------------------------------------------
class GRYCE_API LightProbeGroup : public Component {
public:
    int grid_x = 2;
    int grid_y = 2;
    int grid_z = 2;
    math::Vector3f size = math::Vector3f(10.0f, 10.0f, 10.0f);
    float intensity = 1.0f;

    LightProbeGroup() = default;

    const char* type() const override { return "LightProbeGroup"; }

    void serialize(nlohmann::json& out) const override {
        out["grid_x"] = grid_x;
        out["grid_y"] = grid_y;
        out["grid_z"] = grid_z;
        out["size"] = { size.x, size.y, size.z };
        out["intensity"] = intensity;
    }
    void deserialize(const nlohmann::json& in) override {
        grid_x = in.value("grid_x", 2);
        grid_y = in.value("grid_y", 2);
        grid_z = in.value("grid_z", 2);
        auto s = in.value("size", std::vector<float>{10, 10, 10});
        if (s.size() >= 3) size = math::Vector3f(s[0], s[1], s[2]);
        intensity = in.value("intensity", 1.0f);
    }
};

} // namespace gryce_engine::components