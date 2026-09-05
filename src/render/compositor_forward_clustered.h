#pragma once

#include <memory>

#include "render/export.h"
#include "render/renderer_compositor.h"
#include "render/renderer_scene_render.h"
#include "render/renderer_canvas_render.h"
#include "render/renderer_fog.h"
#include "render/renderer_gi.h"
#include "render/rendering_device.h"
#include "render/storage_rd/light_storage.h"
#include "render/storage_rd/material_storage.h"
#include "render/storage_rd/mesh_storage.h"
#include "render/storage_rd/texture_storage.h"
#include "render/storage_rd/particles_storage.h"
#include "render/storage_rd/utilities.h"

namespace gryce_engine::render {

class RenderContext;

// ---------------------------------------------------------------------------
// CompositorForwardClustered — Forward Clustered 渲染器组合器
// 类似于 Godot 的 RendererCompositorRD，使用 Forward Clustered 管线
// 实现桌面级 PBR 渲染，支持 Cluster 光照剔除、阴影、后处理等。
// ---------------------------------------------------------------------------
class GRYCE_RENDERER_API CompositorForwardClustered : public RendererCompositor {
public:
    CompositorForwardClustered();
    ~CompositorForwardClustered() override;

    // RendererCompositor 接口
    bool initialize(RenderContext* ctx) override;
    void finalize() override;
    void begin_frame(double frame_step) override;
    void end_frame(bool present) override;

    RendererCanvasRender* get_canvas() override { return canvas_render_.get(); }
    RendererSceneRender* get_scene() override { return scene_render_.get(); }
    RendererFog* get_fog() override { return fog_.get(); }
    RendererGI* get_gi() override { return gi_.get(); }

    RendererLightStorage* get_light_storage() override { return light_storage_.get(); }
    RendererMaterialStorage* get_material_storage() override { return material_storage_.get(); }
    RendererMeshStorage* get_mesh_storage() override { return mesh_storage_.get(); }
    RendererTextureStorage* get_texture_storage() override { return texture_storage_.get(); }
    RendererParticlesStorage* get_particles_storage() override { return nullptr; }
    RendererUtilities* get_utilities() override { return nullptr; }

    RenderingDevice* get_device() override { return device_.get(); }
    const char* name() const override { return "CompositorForwardClustered"; }

private:
    std::unique_ptr<RenderingDevice> device_;
    std::unique_ptr<RendererCanvasRender> canvas_render_;
    std::unique_ptr<RendererSceneRender> scene_render_;
    std::unique_ptr<RendererFog> fog_;
    std::unique_ptr<RendererGI> gi_;

    // Storage 系统
    std::unique_ptr<RendererLightStorage> light_storage_;
    std::unique_ptr<RendererMaterialStorage> material_storage_;
    std::unique_ptr<RendererMeshStorage> mesh_storage_;
    std::unique_ptr<RendererTextureStorage> texture_storage_;
    bool initialized_ = false;
};

} // namespace gryce_engine::render