#pragma once

#include <memory>
#include <string>
#include <vector>

#include <imgui.h>

#include "ecs/world.h"
#include "math/camera.h"
#include "platform/input.h"
#include "platform/window.h"
#include "render/render_context.h"
#include "render/render_pipeline.h"
#include "render/opengl/imgui_renderer.h"
#include "scene/scene.h"

#include "file_explorer_panel.h"
#include "hierarchy_panel.h"
#include "inspector_panel.h"
#include "console_panel.h"
#include "command_stack.h"
#include "play_mode_manager.h"
#include "scene_manager.h"
#include "editor_settings.h"
#include "i18n.h"
#include "splash_process.h"

namespace gryce_engine::editor {

// 视口左侧工具模式
enum class ViewportTool : int {
    Select = 0,
    Move,
    Rotate,
    Scale
};

// ---------------------------------------------------------------------------
// EditorApp — 编辑器主应用
// 管理窗口创建、渲染上下文、ImGui Docking 布局和编辑器面板。
// ---------------------------------------------------------------------------
class EditorApp {
public:
    EditorApp() = default;
    ~EditorApp();

    EditorApp(const EditorApp&) = delete;
    EditorApp& operator=(const EditorApp&) = delete;

    bool init(int argc, char* argv[]);
    int run();
    void shutdown();

private:
    void init_docking_layout(bool force = false);
    void render_editor_ui();

    void render_viewport();
    void render_viewport_tool_buttons();
    void render_gizmo();
    void handle_viewport_click_to_select();
    void handle_tool_shortcuts();
    void render_hierarchy();
    void render_inspector();
    void render_console();
    void render_toolbar();
    void render_status_bar();
    void render_menu_bar();
    void render_settings_panel();
    void render_editor_settings_panel();
    void render_scene_tab_bar();

    void handle_shortcuts();
    void apply_editor_font();
    void create_default_scene_content(scene::Scene* scene);
    void open_scene_dialog();
    void save_scene_dialog();
    void start_play();
    void stop_play();
    void pause_scene_transition();
    void resume_scene_transition();

    void update_editor_camera(float dt);
    void collect_scene_lights();
    // 渲染设置面板状态
    bool show_render_settings_ = false;
    // 渲染效果状态
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
    float dof_blur_amount_ = 4.0f;
    float motion_blur_amount_ = 0.5f;
    float fog_density_ = 0.01f;
    float fog_height_ = 20.0f;
    math::Vector3f fog_color_ = math::Vector3f(0.5f, 0.5f, 0.5f);
    float gi_indirect_intensity_ = 1.0f;

    void apply_deferred_deletes();
    void resize_viewport_if_needed(int width, int height);
    uint64_t viewport_texture_id() const;

    std::unique_ptr<platform::Window> window_;
    bool vulkan_ = true;
    bool vulkan_validation_ = false;
    bool running_ = false;
    std::string project_arg_;

    std::unique_ptr<render::RenderContext> render_ctx_;
    // 启动画面子进程：init 阶段由独立进程渲染加载动画，init 完成后通过 marker 通知其退出。
    std::unique_ptr<SplashProcess> splash_;
    std::unique_ptr<render::RenderPipeline> pipeline_;
    std::unique_ptr<render::ImGuiRenderer> imgui_;
    std::unique_ptr<ecs::World> world_;
    std::unique_ptr<platform::InputManager> input_;
    scene::Scene* scene_ = nullptr;

    SceneManager scene_manager_;
    CommandStack command_stack_;
    PlayModeManager play_mode_;
    ConsolePanel console_;
    HierarchyPanel hierarchy_;
    InspectorPanel inspector_;
    FileExplorerPanel file_explorer_;

    math::Camera editor_camera_;
    float editor_camera_yaw_ = -135.0f;
    float editor_camera_pitch_ = -28.0f;
    float editor_camera_distance_ = 8.0f;
    math::Vector3f editor_camera_target_ = math::Vector3f(0.0f, 1.0f, 0.0f);
    std::vector<render::RenderPipeline::Light> editor_lights_;

    bool viewport_size_valid_ = false;
    ImVec2 viewport_size_{};
    bool viewport_hovered_ = false;
    int viewport_texture_width_ = 0;
    int viewport_texture_height_ = 0;
    bool viewport_resize_pending_ = false;

    bool show_demo_window_ = false;
    bool show_metrics_window_ = false;
    bool show_editor_settings_ = false;

    ViewportTool current_tool_ = ViewportTool::Select;
};

} // namespace gryce_engine::editor
