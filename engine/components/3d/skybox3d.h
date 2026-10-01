#pragma once

#include <string>

#include "export.h"
#include "components/component.h"

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// Skybox3D — 3D 天空盒组件
//
// 纯数据组件：不新增着色器，也不直接持有 GPU 资源，而是把 texture_path /
// environment_path / exposure / visible 映射到 RenderPipeline 已有的天空盒 +
// IBL 环境 API（见 RenderPipeline::collect_skybox）。渲染管线每帧收集启用的
// Skybox3D 并只在配置变化时重配置（set_skybox / set_environment_hdr 会重建
// cubemap + 预滤波 IBL + 管线，代价高，不能逐帧重复调用）。
// ---------------------------------------------------------------------------
class GRYCE_API Skybox3D : public Component {
public:
    std::string texture_path;
    std::string environment_path;
    float exposure = 1.0f;
    bool visible = true;

    Skybox3D() = default;

    const char* type() const override { return "Skybox3D"; }

    void serialize(nlohmann::json& out) const override {
        out["texture_path"] = texture_path;
        out["environment_path"] = environment_path;
        out["exposure"] = exposure;
        out["visible"] = visible;
    }
    void deserialize(const nlohmann::json& in) override {
        texture_path = in.value("texture_path", "");
        environment_path = in.value("environment_path", "");
        exposure = in.value("exposure", 1.0f);
        visible = in.value("visible", true);
    }
};

} // namespace gryce_engine::components