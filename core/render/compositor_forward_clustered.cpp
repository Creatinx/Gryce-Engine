#include "render/compositor_forward_clustered.h"
#include "render/renderer_rd/forward_clustered/render_forward_clustered.h"
#include "render/renderer_canvas_render_rd.h"
#include "render/render_context.h"
#include "render/renderer_compositor.h"
#include "render/storage_rd/mesh_storage_impl.h"
#include "render/storage_rd/material_storage_impl.h"
#include "render/storage_rd/texture_storage_impl.h"
#include "render/storage_rd/light_storage_impl.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::render {

CompositorForwardClustered::CompositorForwardClustered() = default;

CompositorForwardClustered::~CompositorForwardClustered() {
    finalize();
}

static RendererCompositor* create_compositor_forward_clustered(RenderContext* ctx) {
    auto* compositor = new CompositorForwardClustered();
    if (!compositor->initialize(ctx)) {
        delete compositor;
        return nullptr;
    }
    return compositor;
}

bool CompositorForwardClustered::initialize(RenderContext* ctx) {
    if (initialized_) return true;

    // 创建 Scene Renderer — Forward Clustered
    // 使用 forward_clustered 子目录下的 shader 文件
    scene_render_ = std::make_unique<RenderForwardClustered>();
    if (!scene_render_->init(ctx, "res:/shaders/forward_clustered")) {
        GLOG_ERROR("CompositorForwardClustered: failed to init RenderForwardClustered");
        return false;
    }

    // 创建 Canvas Renderer
    canvas_render_ = std::make_unique<RendererCanvasRenderRD>();
    if (!canvas_render_->init(ctx)) {
        GLOG_ERROR("CompositorForwardClustered: failed to init CanvasRenderer");
        return false;
    }

    // 创建 Storage 系统
    mesh_storage_ = std::make_unique<MeshStorageImpl>(ctx);
    material_storage_ = std::make_unique<MaterialStorageImpl>();
    texture_storage_ = std::make_unique<TextureStorageImpl>(ctx);
    light_storage_ = std::make_unique<LightStorageImpl>(ctx);

    // 创建 Fog / GI 桩
    fog_ = std::make_unique<RendererFog>();
    gi_ = std::make_unique<RendererGI>();

    // 注册工厂函数
    set_create_func(create_compositor_forward_clustered);
    set_singleton(this);

    initialized_ = true;
    GLOG_INFO("CompositorForwardClustered initialized (Forward Clustered renderer)");
    return true;
}

void CompositorForwardClustered::finalize() {
    if (initialized_) {
        if (get_singleton() == this) {
            set_singleton(nullptr);
        }
        scene_render_.reset();
        canvas_render_.reset();
        light_storage_.reset();
        material_storage_.reset();
        mesh_storage_.reset();
        texture_storage_.reset();
        fog_.reset();
        gi_.reset();
        device_.reset();
        initialized_ = false;
    }
}

void CompositorForwardClustered::begin_frame(double frame_step) {
    (void)frame_step;

    // 每帧开始前更新所有 Storage 的 GPU 缓冲
    if (light_storage_) {
        light_storage_->update_buffers();

        // 将光源 SSBO 句柄传递给 Forward Clustered 渲染器
        auto* fc_renderer = static_cast<RenderForwardClustered*>(scene_render_.get());
        auto* light_impl = static_cast<LightStorageImpl*>(light_storage_.get());
        if (fc_renderer && light_impl) {
            fc_renderer->set_light_buffer(light_impl->light_buffer());
        }
    }
    if (mesh_storage_) {
        mesh_storage_->update_buffers();
    }
    if (material_storage_) {
        material_storage_->update_buffers();
    }
    if (texture_storage_) {
        texture_storage_->update_buffers();
    }
}

void CompositorForwardClustered::end_frame(bool present) {
    (void)present;
}

} // namespace gryce_engine::render