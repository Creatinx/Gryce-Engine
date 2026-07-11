#pragma once

#include "ecs/system.h"
#include "render/render2d.h"

namespace gryce_engine::ecs {

// ---------------------------------------------------------------------------
// RenderSystem2D — 2D 渲染系统
// 遍历所有挂载 Component2D 的 Entity 并绘制。
// ---------------------------------------------------------------------------
class RenderSystem2D : public ISystem {
public:
    explicit RenderSystem2D(render::IRenderer2D* renderer)
        : renderer_(renderer) {}

    const char* name() const override { return "RenderSystem2D"; }
    Phase phase() const override { return Phase::Render; }

    void on_render(scene::Scene& scene, render::RenderContext& ctx) override;

    // 上一帧是否真正执行了渲染（用于 Dirty-Frame 优化）
    bool rendered_last_frame() const { return rendered_last_frame_; }

private:
    render::IRenderer2D* renderer_ = nullptr;
    uint64_t last_hash_ = 0;
    bool first_frame_ = true;
    bool rendered_last_frame_ = true;

    uint64_t compute_scene_hash(scene::Scene& scene);
};

} // namespace gryce_engine::ecs
