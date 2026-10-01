#pragma once

#include "components/component.h"
#include "components/3d/particle_system_3d.h"
#include "components/3d/trail_renderer.h"
#include "components/3d/line_renderer_3d.h"
#include "components/3d/instanced_mesh_renderer.h"
#include "components/3d/billboard.h"
#include "components/3d/text_mesh3d.h"
#include "components/3d/skybox3d.h"
#include "components/3d/reflection_probe.h"
#include "components/3d/light_probe_group.h"
#include "components/3d/lod_group.h"
#include "components/3d/fog_volume.h"
#include "components/3d/volumetric_light.h"
#include "components/decal.h"
#include "math/math.h"
#include "render/render2d.h"

#include <string>
#include <vector>

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// Animator — 动画状态机组件（数据层；AnimatorSystem 后续消费）
// 驱动 SkinnedMeshRenderer 的片段切换、混合与播放参数。
// ---------------------------------------------------------------------------
class Animator : public Component {
public:
    std::string clip_name;
    bool playing = true;
    bool loop = true;
    float speed = 1.0f;
    float time = 0.0f;
    float blend_duration = 0.2f;

    Animator() = default;
    const char* type() const override { return "Animator"; }

    void serialize(nlohmann::json& out) const override {
        out["clip_name"] = clip_name;
        out["playing"] = playing;
        out["loop"] = loop;
        out["speed"] = speed;
        out["time"] = time;
        out["blend_duration"] = blend_duration;
    }
    void deserialize(const nlohmann::json& in) override {
        clip_name = in.value("clip_name", "");
        playing = in.value("playing", true);
        loop = in.value("loop", true);
        speed = in.value("speed", 1.0f);
        time = in.value("time", 0.0f);
        blend_duration = in.value("blend_duration", 0.2f);
    }
};

// ---------------------------------------------------------------------------
// ParticleSystem3D — 见 components/3d/particle_system_3d.h
// （组件已独立成 .h/.cpp 以便实现 CPU 模拟与 GPU 上传）
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// TrailRenderer — 见 components/3d/trail_renderer.h
// （组件已独立成 .h/.cpp 以便实现 CPU 采样历史与 GPU 上传）
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// LineRenderer3D — 见 components/3d/line_renderer_3d.h
// （组件已独立成 .h/.cpp 以便实现段实例流展开与 GPU 上传）
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Decal — 见 components/decal.h
// （组件已独立成 .h，避免与此前同名重复定义产生 ODR 冲突；
//   渲染路径 RenderForwardClustered 读取的即为该定义）
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Billboard — 见 components/3d/billboard.h
// （组件已独立成 .h/.cpp 以便实现实例流展开与 GPU 上传）
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// TextMesh3D — 见 components/3d/text_mesh3d.h
// （组件已独立成 .h/.cpp 以便实现字形网格构建与 GPU 上传）
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Skybox3D — 见 components/3d/skybox3d.h
// （纯数据组件，接入 RenderPipeline 已有的天空盒 / IBL API）
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// ReflectionProbe — 见 components/3d/reflection_probe.h
// （纯数据组件，接入 RenderPipeline 已有的探针系统 ReflectionProbeRD）
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// LightProbeGroup — 见 components/3d/light_probe_group.h
// （纯数据组件，从管线已有环境 IBL 推导辐照度并入间接光路径）
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// FogVolume — 见 components/3d/fog_volume.h
// （纯数据组件，接入 RenderPipeline 已有的全局体积雾 VolumetricFog_RD）
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// VolumetricLight — 见 components/3d/volumetric_light.h
// （自包含发射网格组件，程序化生成锥体光柱并投递 GPU 上传）
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// LODGroup — 见 components/3d/lod_group.h
// （自包含发射网格组件，按屏幕覆盖率选 LOD，并入不透明绘制列表）
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// InstancedMeshRenderer — 见 components/3d/instanced_mesh_renderer.h
// （组件已独立成 .h/.cpp 以便实现实例变换生成与 GPU 实例流上传）
// ---------------------------------------------------------------------------

} // namespace gryce_engine::components
