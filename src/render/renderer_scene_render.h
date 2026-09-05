#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "render/export.h"
#include "math/math.h"

namespace gryce_engine {
namespace scene { class Scene; }
namespace math { class Camera; }
} // namespace gryce_engine

namespace gryce_engine::render {

class RenderContext;
class RHIRenderTexture;

// ---------------------------------------------------------------------------
// RendererSceneRender — 3D 场景渲染器抽象接口
// 类似于 Godot 的 RendererSceneRender，所有 3D 场景渲染器实现此接口。
// ---------------------------------------------------------------------------
class GRYCE_RENDERER_API RendererSceneRender {
public:
    struct RenderData {
        scene::Scene* scene = nullptr;
        class math::Camera* camera = nullptr;
        int viewport_width = 0;
        int viewport_height = 0;
        float delta_time = 0.0f;
        uint32_t frame_index = 0;
        math::Vector3f ambient_light = math::Vector3f(0.15f, 0.15f, 0.15f);
    };

    virtual ~RendererSceneRender() = default;

    // 生命周期
    virtual bool init(RenderContext* ctx, const std::string& shader_dir = "res:/shaders") = 0;
    virtual void shutdown() = 0;

    // 渲染一帧
    virtual void render_scene(RenderData& data) = 0;

    // 视口输出（编辑器离屏渲染）
    virtual void set_viewport_output_enabled(bool enabled) = 0;
    virtual bool viewport_output_enabled() const = 0;
    virtual class ITexture* viewport_color_texture() const = 0;
    virtual bool resize_render_targets(int width, int height) = 0;

    // 热重载
    virtual bool hot_reload() = 0;
    virtual int poll_shader_hot_reload(RenderContext& ctx) = 0;
};

} // namespace gryce_engine::render