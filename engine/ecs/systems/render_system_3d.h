#pragma once

#include "export.h"
#include "ecs/system.h"

namespace gryce_engine::render {
    class RenderPipeline;
} // namespace gryce_engine::render

namespace gryce_engine::ecs {

// ---------------------------------------------------------------------------
// RenderSystem3D — 3D 运行期上传系统。
//
// 注意：场景渲染编排由 GRender_RenderWorld → RenderPipeline 负责。本系统只
// 处理运行期新增网格/蒙皮模型的 GPU 上传，避免形成第二条“RenderingServer”
// 渲染入口。
// ---------------------------------------------------------------------------
class GRYCE_API RenderSystem3D : public ISystem {
public:
    explicit RenderSystem3D(render::RenderPipeline* pipeline = nullptr)
        : pipeline_(pipeline) {}

    const char* name() const override { return "RenderSystem3D"; }
    Phase phase() const override { return Phase::Render; }

    void on_render(scene::Scene& scene, render::RenderContext& ctx) override;

private:
    render::RenderPipeline* pipeline_ = nullptr;
};

} // namespace gryce_engine::ecs
