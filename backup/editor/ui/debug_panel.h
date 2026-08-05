#pragma once

#include <string>

struct GLFWwindow;

namespace gryce_engine {

namespace scene { class Scene; class Entity; }
namespace math { class Camera; }
namespace platform { class Window; }
namespace render { class RenderContext; class RenderPipeline; }
namespace utils { class FrameLimiter; }

namespace editor::ui {

// ---------------------------------------------------------------------------
// DebugPanel — ImGui 调试面板
// ---------------------------------------------------------------------------
class DebugPanel {
public:
    void show(platform::Window* window, scene::Scene* scene, math::Camera* camera,
              utils::FrameLimiter* frame_limiter, render::RenderContext* render_ctx,
              render::RenderPipeline* pipeline = nullptr);

    scene::Entity* selected_entity() const { return selected_entity_; }
    void clear_selection() { selected_entity_ = nullptr; }

    bool invert_mouse_y() const { return invert_mouse_y_; }
    bool swap_space_ctrl() const { return swap_space_ctrl_; }
    bool disable_cull() const { return disable_cull_; }

private:
    void draw_scene_hierarchy(scene::Entity* entity);
    void draw_entity_inspector(scene::Entity* entity);

    scene::Entity* selected_entity_ = nullptr;

    bool invert_mouse_y_ = false;  // 默认标准 FPS：鼠标上移抬头
    bool swap_space_ctrl_ = false; // 默认标准 FPS：Space=上升、Ctrl=下降
    bool disable_cull_ = false;    // 默认启用剔除，需要时可在 Debug 面板关闭
};

// ---------------------------------------------------------------------------
// ModelLoaderPanel — 模型加载面板
// ---------------------------------------------------------------------------
class ModelLoaderPanel {
public:
    // 返回 true 表示本帧点击了 Load Model（需要主线程暂停渲染线程上传 GPU 资源）
    bool show(scene::Scene* scene);

private:
    char path_buffer_[256] = "res:/models/cube.obj";
};

} // namespace editor::ui

} // namespace gryce_engine
