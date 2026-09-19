#pragma once

#include <memory>
#include <string>

#include "render/export.h"
#include "render/renderer_scene_render.h"

namespace gryce_engine::render {

class RenderContext;
class RenderPipeline;
class ITexture;

// ---------------------------------------------------------------------------
// RenderForwardBasic — 包装现有 RenderPipeline 的适配器
// 将现有的 RenderPipeline 包装为 RendererSceneRender 接口，
// 作为 Godot 式渲染器架构的过渡实现。
// 后续会被 RenderForwardClustered 替代。
// ---------------------------------------------------------------------------
class GRYCE_RENDERER_API RenderForwardBasic : public RendererSceneRender {
public:
    RenderForwardBasic();
    ~RenderForwardBasic() override;

    // RendererSceneRender 接口
    bool init(RenderContext* ctx, const std::string& shader_dir = "res:/shaders") override;
    void shutdown() override;
    void render_scene(RenderData& data) override;

    void set_viewport_output_enabled(bool enabled) override;
    bool viewport_output_enabled() const override;
    ITexture* viewport_color_texture() const override;
    bool resize_render_targets(int width, int height) override;

    bool hot_reload() override;
    int poll_shader_hot_reload(RenderContext& ctx) override;

    // 访问底层 RenderPipeline
    RenderPipeline* native_pipeline() const { return pipeline_.get(); }

private:
    std::unique_ptr<RenderPipeline> pipeline_;
    bool initialized_ = false;
};

} // namespace gryce_engine::render