#pragma once

#include "ecs/system.h"

namespace gryce_engine {
namespace render { class RenderPipeline; }
} // namespace gryce_engine

namespace gryce_engine::ecs {

// ---------------------------------------------------------------------------
// RenderSystem3D — 3D 网格渲染系统
// 通过 RenderPipeline 渲染所有 MeshRenderer + Transform 实体。
// ---------------------------------------------------------------------------
class RenderSystem3D : public ISystem {
public:
    explicit RenderSystem3D(render::RenderPipeline* pipeline)
        : pipeline_(pipeline) {}

    const char* name() const override { return "RenderSystem3D"; }
    Phase phase() const override { return Phase::Render; }

    void on_render(scene::Scene& scene, render::RenderContext& ctx) override;

private:
    render::RenderPipeline* pipeline_ = nullptr;
};

} // namespace gryce_engine::ecs
