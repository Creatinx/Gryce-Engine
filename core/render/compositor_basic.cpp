#include "render/compositor_basic.h"
#include "render/render_forward_basic.h"
#include "render/renderer_canvas_render_rd.h"
#include "render/render_context.h"
#include "render/renderer_compositor.h"

namespace gryce_engine::render {

CompositorBasic::CompositorBasic() = default;

CompositorBasic::~CompositorBasic() {
    finalize();
}

static RendererCompositor* create_compositor_basic(RenderContext* ctx) {
    auto* compositor = new CompositorBasic();
    if (!compositor->initialize(ctx)) {
        delete compositor;
        return nullptr;
    }
    return compositor;
}

bool CompositorBasic::initialize(RenderContext* ctx) {
    if (initialized_) return true;

    // 创建 Scene Renderer
    scene_render_ = std::make_unique<RenderForwardBasic>();
    if (!scene_render_->init(ctx)) {
        return false;
    }

    // 创建 Canvas Renderer
    canvas_render_ = std::make_unique<RendererCanvasRenderRD>();
    if (!canvas_render_->init(ctx)) {
        return false;
    }

    // 创建 Fog / GI 桩
    fog_ = std::make_unique<RendererFog>();
    gi_ = std::make_unique<RendererGI>();

    // 注册工厂函数
    set_create_func(create_compositor_basic);
    set_singleton(this);

    initialized_ = true;
    return true;
}

void CompositorBasic::finalize() {
    if (initialized_) {
        if (get_singleton() == this) {
            set_singleton(nullptr);
        }
        scene_render_.reset();
        canvas_render_.reset();
        fog_.reset();
        gi_.reset();
        device_.reset();
        initialized_ = false;
    }
}

void CompositorBasic::begin_frame(double frame_step) {
    (void)frame_step;
}

void CompositorBasic::end_frame(bool present) {
    (void)present;
}

} // namespace gryce_engine::render