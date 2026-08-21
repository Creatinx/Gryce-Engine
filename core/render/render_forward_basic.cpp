#include "render/render_forward_basic.h"
#include "render/render_pipeline.h"
#include "render/render_context.h"
#include "render/texture.h"
#include "math/camera.h"
#include "scene/scene.h"

namespace gryce_engine::render {

RenderForwardBasic::RenderForwardBasic()
    : pipeline_(std::make_unique<RenderPipeline>()) {
}

RenderForwardBasic::~RenderForwardBasic() {
    shutdown();
}

bool RenderForwardBasic::init(RenderContext* ctx, const std::string& shader_dir) {
    if (initialized_) return true;
    if (!pipeline_->init(ctx, shader_dir)) {
        return false;
    }
    initialized_ = true;
    return true;
}

void RenderForwardBasic::shutdown() {
    if (initialized_) {
        pipeline_->shutdown();
        initialized_ = false;
    }
}

void RenderForwardBasic::render_scene(RenderData& data) {
    if (!initialized_ || !data.scene || !data.camera) return;

    pipeline_->set_camera(*data.camera);
    pipeline_->set_viewport(data.viewport_width, data.viewport_height);

    // 委托给 RenderPipeline
    pipeline_->render_scene(*data.scene, *static_cast<RenderContext*>(nullptr));
    // TODO: 需要实际传递 RenderContext，当前 render_scene 签名需要改进
}

void RenderForwardBasic::set_viewport_output_enabled(bool enabled) {
    pipeline_->set_viewport_output_enabled(enabled);
}

bool RenderForwardBasic::viewport_output_enabled() const {
    return pipeline_->viewport_output_enabled();
}

ITexture* RenderForwardBasic::viewport_color_texture() const {
    return pipeline_->viewport_color_texture();
}

bool RenderForwardBasic::resize_render_targets(int width, int height) {
    return pipeline_->resize_render_targets(width, height);
}

bool RenderForwardBasic::hot_reload() {
    return pipeline_->hot_reload();
}

int RenderForwardBasic::poll_shader_hot_reload(RenderContext& ctx) {
    return pipeline_->poll_shader_hot_reload(ctx);
}

} // namespace gryce_engine::render