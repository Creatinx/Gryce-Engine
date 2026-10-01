#pragma once

#include "export.h"
#include "components/component.h"
#include "math/math.h"

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// ReflectionProbe — 局部反射探针
//
// 与 Skybox3D 同类：不持有独立 GPU 资源，也不新增着色器，而是由
// RenderPipeline::collect_reflection_probes 把组件配置映射到引擎既有的探针系统
// （ReflectionProbeRD，经 RenderPipeline::probe_system() 暴露）：每个启用的组件
// 在探针系统里对应一个槽位，写入立方体范围/强度，并在配置变化或 realtime 节流
// 到期时请求重新捕获。探针的 irradiance/prefilter 随后由 bind_probe_ibl 覆盖全局
// IBL（render_mesh_internal 中按物体位置取最近探针）。
//
// 诚实说明（能力边界，未另建并行探针系统）：
//  1. ReflectionProbeRD 的探针分辨率固定为 k_probe_resolution=256，组件的
//     resolution 字段无法真正驱动它，仅保留用于序列化与编辑器显示。
//  2. ProbeData.intensity 会被写入探针数据（见 collect_reflection_probes），
//     但当前渲染侧只消费探针的 irradiance/prefilter（bind_probe_ibl 不读
//     intensity），因此强度目前不影响画面。这是一处已记录的能力缺口。
//  3. ReflectionProbeRD::capture_probe / prefilter_probe 是引擎既有的简化实现
//     （以固定灰度环境预滤波，不真正重绘场景）；本组件只是调用它们，不改变其行为。
// ---------------------------------------------------------------------------
class GRYCE_API ReflectionProbe : public Component {
public:
    int resolution = 256;
    math::Vector3f box_extents = math::Vector3f(10.0f, 10.0f, 10.0f);
    float intensity = 1.0f;
    bool realtime = false;

    ReflectionProbe() = default;

    const char* type() const override { return "ReflectionProbe"; }

    void serialize(nlohmann::json& out) const override {
        out["resolution"] = resolution;
        out["box_extents"] = { box_extents.x, box_extents.y, box_extents.z };
        out["intensity"] = intensity;
        out["realtime"] = realtime;
    }
    void deserialize(const nlohmann::json& in) override {
        resolution = in.value("resolution", 256);
        auto b = in.value("box_extents", std::vector<float>{10, 10, 10});
        if (b.size() >= 3) box_extents = math::Vector3f(b[0], b[1], b[2]);
        intensity = in.value("intensity", 1.0f);
        realtime = in.value("realtime", false);
    }
};

} // namespace gryce_engine::components