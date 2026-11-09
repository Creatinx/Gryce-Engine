#pragma once

#include <memory>

#include "render/rhi_handle.h"
#include "ui/ui_input.h"

namespace gryce_engine {
namespace scene { class Scene; }
namespace math { class Camera; }
namespace render {

class RenderContext;
class IRenderer2D;

// 2D 画布管线：拥有 IRenderer2D，负责 2D 场景批次和屏幕覆盖层。
// 它不拥有交换链、3D 目标或 UI 状态；3D 与 UI 管线只共享同一 RenderContext。
class Canvas2DPipeline {
public:
    Canvas2DPipeline();
    ~Canvas2DPipeline();

    bool initialize(RenderContext* ctx);
    void shutdown();

    IRenderer2D* renderer() const { return renderer_.get(); }
    bool valid() const { return renderer_ != nullptr; }

    void render(scene::Scene* scene,
                int screen_w, int screen_h,
                bool draw_fps_fallback,
                float fps, float frame_ms,
                const ui::UIInputFrame& ui_input,
                const math::Camera* camera);

    void ensure_text_raster_size(float physical_px);

    void set_viewport_output_enabled(bool enabled) { viewport_output_enabled_ = enabled; }
    bool viewport_output_enabled() const { return viewport_output_enabled_; }
    RHITextureHandle viewport_texture() const { return viewport_texture_; }
    int viewport_width() const { return viewport_width_; }
    int viewport_height() const { return viewport_height_; }

private:
    bool ensure_viewport_target(int width, int height);
    void destroy_viewport_target();

    std::unique_ptr<IRenderer2D> renderer_;
    RenderContext* ctx_ = nullptr;
    bool viewport_output_enabled_ = false;
    RHITextureHandle viewport_texture_;
    RHIFramebufferHandle viewport_framebuffer_;
    int viewport_width_ = 0;
    int viewport_height_ = 0;
};

} // namespace render
} // namespace gryce_engine
