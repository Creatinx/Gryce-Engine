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

    // 渲染效果开关
    bool ssr_enabled() const { return ssr_enabled_; }
    bool dof_enabled() const { return dof_enabled_; }
    bool motion_blur_enabled() const { return motion_blur_enabled_; }
    bool fog_enabled() const { return fog_enabled_; }
    bool water_enabled() const { return water_enabled_; }
    bool point_shadow_enabled() const { return point_shadow_enabled_; }
    bool shadow_atlas_enabled() const { return shadow_atlas_enabled_; }
    int shadow_mode() const { return shadow_mode_; }
    int gi_mode() const { return gi_mode_; }
    float dof_focus_distance() const { return dof_focus_distance_; }
    float dof_focus_range() const { return dof_focus_range_; }
    float motion_blur_amount() const { return motion_blur_amount_; }
    float fog_density() const { return fog_density_; }
    float fog_height() const { return fog_height_; }

    bool invert_mouse_y() const { return invert_mouse_y_; }
    bool swap_space_ctrl() const { return swap_space_ctrl_; }
    bool disable_cull() const { return disable_cull_; }

    // 调试面板请求重建渲染管线：按钮点击后由主循环在 present() 之后消费并执行
    //（重建需要暂停渲染线程，不能在 ImGui 帧内直接调用）。
    bool consume_pipeline_reload_request() {
        const bool requested = pipeline_reload_requested_;
        pipeline_reload_requested_ = false;
        return requested;
    }

private:
    void draw_scene_hierarchy(scene::Entity* entity);
    void draw_entity_inspector(scene::Entity* entity);

    scene::Entity* selected_entity_ = nullptr;
    bool pipeline_reload_requested_ = false;

    bool invert_mouse_y_ = false;  // 默认标准 FPS：鼠标上移抬头
    bool swap_space_ctrl_ = false; // 默认标准 FPS：Space=上升、Ctrl=下降
    bool disable_cull_ = false;    // 默认启用剔除，需要时可在 Debug 面板关闭

    bool ssr_enabled_ = false;
    bool dof_enabled_ = false;
    bool motion_blur_enabled_ = false;
    bool fog_enabled_ = false;
    bool water_enabled_ = false;
    bool point_shadow_enabled_ = true;
    bool shadow_atlas_enabled_ = false;
    int shadow_mode_ = 0; // 0=PCF, 1=VSM, 2=ESM
    int gi_mode_ = 0;     // 0=None, 1=SDFGI, 2=VoxelGI
    float dof_focus_distance_ = 10.0f;
    float dof_focus_range_ = 5.0f;
    float motion_blur_amount_ = 0.5f;
    float fog_density_ = 0.01f;
    float fog_height_ = 20.0f;
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
