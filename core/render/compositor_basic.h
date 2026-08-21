#pragma once

#include <memory>

#include "render/export.h"
#include "render/renderer_compositor.h"
#include "render/renderer_scene_render.h"
#include "render/renderer_canvas_render.h"
#include "render/renderer_fog.h"
#include "render/renderer_gi.h"
#include "render/rendering_device.h"

namespace gryce_engine::render {

class RenderContext;

// ---------------------------------------------------------------------------
// CompositorBasic — 基础渲染器组合器
// 将现有的 RenderPipeline 和 Renderer2D 包装为 RendererCompositor 接口。
// 作为 Godot 式渲染器架构的过渡实现。
// ---------------------------------------------------------------------------
class GRYCE_RENDERER_API CompositorBasic : public RendererCompositor {
public:
    CompositorBasic();
    ~CompositorBasic() override;

    // RendererCompositor 接口
    bool initialize(RenderContext* ctx) override;
    void finalize() override;
    void begin_frame(double frame_step) override;
    void end_frame(bool present) override;

    RendererCanvasRender* get_canvas() override { return canvas_render_.get(); }
    RendererSceneRender* get_scene() override { return scene_render_.get(); }
    RendererFog* get_fog() override { return fog_.get(); }
    RendererGI* get_gi() override { return gi_.get(); }

    RendererLightStorage* get_light_storage() override { return nullptr; }
    RendererMaterialStorage* get_material_storage() override { return nullptr; }
    RendererMeshStorage* get_mesh_storage() override { return nullptr; }
    RendererTextureStorage* get_texture_storage() override { return nullptr; }
    RendererParticlesStorage* get_particles_storage() override { return nullptr; }
    RendererUtilities* get_utilities() override { return nullptr; }

    RenderingDevice* get_device() override { return device_.get(); }
    const char* name() const override { return "CompositorBasic"; }

private:
    std::unique_ptr<RenderingDevice> device_;
    std::unique_ptr<RendererCanvasRender> canvas_render_;
    std::unique_ptr<RendererSceneRender> scene_render_;
    std::unique_ptr<RendererFog> fog_;
    std::unique_ptr<RendererGI> gi_;
    bool initialized_ = false;
};

} // namespace gryce_engine::render