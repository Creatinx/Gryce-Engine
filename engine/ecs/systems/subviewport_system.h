#pragma once

#include <memory>

#include "ecs/system.h"
#include "render/render_pipeline.h"

namespace gryce_engine::ecs {

// ---------------------------------------------------------------------------
// SubViewportSystem — 运行时 3D→2D 视口
// 每帧（在 RenderSystem3D 之后、RenderSystem2D 之前）用独立 RenderPipeline
// 把场景渲染到离屏纹理，并注入同名 Sprite2D。仅在同步模式 + OpenGL 下生效，
// 无 SubViewport 组件时零开销直接返回。
// ---------------------------------------------------------------------------
class GRYCE_API SubViewportSystem : public ISystem {
public:
    const char* name() const override { return "SubViewportSystem"; }

    void on_render(scene::Scene& scene, render::RenderContext& ctx) override;

private:
    std::unique_ptr<render::RenderPipeline> pipeline_;
    bool init_attempted_ = false;
    int last_width_ = 0;
    int last_height_ = 0;
};

} // namespace gryce_engine::ecs
