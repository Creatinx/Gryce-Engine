// render_forward_basic.cpp — 前向基础渲染路径：对 RenderPipeline 的薄封装，
// 负责 init / shutdown 生命周期，并把 render_scene 连同相机与视口设置一起
// 委托给管线，供上层 Renderer 直接调用。
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

    // 委托给 RenderPipeline（当前传入空 RenderContext：render_scene 的签名
    // 尚未带上上下文，属于已知待改进项）。
    pipeline_->render_scene(*data.scene, *static_cast<RenderContext*>(nullptr));
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