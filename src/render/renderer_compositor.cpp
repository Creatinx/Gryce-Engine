#include "render/renderer_compositor.h"

namespace gryce_engine::render {

RendererCompositor* RendererCompositor::singleton_ = nullptr;
RendererCompositor::CreateFunc RendererCompositor::create_func_ = nullptr;

RendererCompositor* RendererCompositor::get_singleton() {
    return singleton_;
}

void RendererCompositor::set_singleton(RendererCompositor* compositor) {
    singleton_ = compositor;
}

void RendererCompositor::set_create_func(CreateFunc func) {
    create_func_ = func;
}

RendererCompositor* RendererCompositor::create(RenderContext* ctx) {
    if (create_func_) {
        return create_func_(ctx);
    }
    return nullptr;
}

} // namespace gryce_engine::render