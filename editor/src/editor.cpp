#include "editor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#else
#include <unistd.h>
#include <climits>
#endif

#include <GLFW/glfw3.h>
#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif
#include <imgui.h>
#include <imgui_internal.h>
#include <ImGuizmo.h>

#include "components/component_factory.h"
#include "components/camera.h"
#include "components/light.h"
#include "components/mesh_renderer.h"
#include "components/transform.h"
#include "ecs/systems/render_system_3d.h"
#include "render/core_shaders.h"
#include "render/imgui_backend.h"
#include "resources/project.h"
#include "scene/scene_serializer.h"
#include "utils/frame_limiter.h"
#include "utils/glog/glog_lib.h"
#include "editor_theme.h"
#include "fluent_components.h"
#include "splash_process.h"

namespace gryce_engine::editor {

namespace {
const char* tr(const char* key) { return I18n::instance().tr(key); }
} // namespace

namespace {

std::filesystem::path find_project_root() {
    std::filesystem::path exe_path;
#ifdef _WIN32
    wchar_t buffer[MAX_PATH];
    if (GetModuleFileNameW(nullptr, buffer, MAX_PATH) > 0) {
        exe_path = std::filesystem::path(buffer);
    }
#else
    char buffer[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buffer, PATH_MAX - 1);
    if (len > 0) {
        buffer[len] = '\0';
        exe_path = std::filesystem::path(buffer);
    }
#endif
    std::filesystem::path dir = exe_path.parent_path();
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        if (std::filesystem::exists(dir / "CMakeLists.txt") &&
            std::filesystem::is_directory(dir / "core")) {
            return dir;
        }
        dir = dir.parent_path();
    }
    return std::filesystem::current_path();
}

#ifdef _WIN32
std::string native_file_dialog(bool save, const std::string& title) {
    char file[1024] = {};
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof(file);
    ofn.lpstrTitle = title.c_str();
    ofn.lpstrFilter =
        "Gryce Scene (*.gesc)\0*.gesc\0All Files (*.*)\0*.*\0";
    ofn.lpstrDefExt = "gesc";
    if (save) {
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
        if (GetSaveFileNameA(&ofn)) {
            return file;
        }
    } else {
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (GetOpenFileNameA(&ofn)) {
            return file;
        }
    }
    return {};
}
#else
std::string native_file_dialog(bool, const std::string&) {
    return {};
}
#endif

} // namespace

EditorApp::~EditorApp() {
    shutdown();
}

bool EditorApp::init(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--vulkan") == 0) {
            vulkan_ = true;
        } else if (std::strcmp(argv[i], "--opengl") == 0) {
            vulkan_ = false;
        } else if (std::strcmp(argv[i], "--vulkan-validation") == 0) {
            vulkan_ = true;
            vulkan_validation_ = true;
        } else if (std::strcmp(argv[i], "--project") == 0 && i + 1 < argc) {
            project_arg_ = argv[++i];
        }
    }

    utils::glog_initialize();
    utils::GLog::instance().set_min_level(utils::LogLevel::Warn);
    utils::GLog::instance().set_logger(
        std::make_unique<utils::MemoryLogSink>(
            std::make_unique<utils::ConsoleLogger>(), 2000));

    // 加载编辑器偏好（窗口尺寸/主题/语言）。
    EditorSettings::instance().load();
    const EditorSettings& settings = EditorSettings::instance();

    // 初始化界面语言（i18n）。
    I18n::instance().set_language(settings.language());

    // 项目根：--project 指定（启动器/命令行）时用指定目录，否则用引擎根目录。
    // res:// 虚拟根 = 项目根。
    const std::filesystem::path engine_root = find_project_root();
    const std::filesystem::path project_root =
        project_arg_.empty() ? engine_root
                             : std::filesystem::path(project_arg_);
    resources::Project::instance().set_root(project_root.string());
    components::register_builtin_components();

    if (!platform::Window::init_sdk()) {
        GLOG_ERROR("Failed to initialize GLFW");
        return false;
    }

    platform::WindowContextType window_ctx =
        vulkan_ ? platform::WindowContextType::NoApi
                : platform::WindowContextType::OpenGL;
    window_ = std::make_unique<platform::Window>(
        "Gryce Engine Editor", settings.window_width(), settings.window_height(),
        platform::WindowMode::Windowed, window_ctx);
    if (!window_ || !window_->is_valid()) {
        GLOG_ERROR("Failed to create editor window");
        platform::Window::shutdown_sdk();
        return false;
    }

    // 恢复上次最大化状态。
    if (settings.window_maximized()) {
        window_->maximize();
    }

    if (!vulkan_) {
        window_->set_vsync(false);
    }

    // 启动画面：隐藏主窗口避免空白闪烁，派生子进程展示加载动画。
    // 子进程拥有独立的 GLFW 窗口/GL 上下文/ImGui 上下文，与主编辑器完全进程级隔离，
    // 不抢占主线程（着色器编译需要）的 context，init 期间并行渲染覆盖空白窗口期。
    window_->set_visible(false);
    splash_ = std::make_unique<SplashProcess>();

    GLFWwindow* glfw_window = window_->native_handle();

    render_ctx_ = std::make_unique<render::RenderContext>();
    auto render_api = vulkan_ ? render::RenderAPI::Vulkan : render::RenderAPI::OpenGL;
    if (vulkan_validation_) {
        render_ctx_->set_validation_enabled(true);
    }
    auto backend = render::create_render_backend(render_api);
    if (!backend || !render_ctx_->init(glfw_window, std::move(backend))) {
        GLOG_ERROR("Failed to initialize render context");
        window_.reset();
        platform::Window::shutdown_sdk();
        return false;
    }

    window_->set_resize_callback([this](int w, int h) {
        render_ctx_->set_viewport(0, 0, w, h);
    });

    imgui_ = std::make_unique<render::ImGuiRenderer>();
    auto imgui_backend = render_ctx_->create_imgui_backend();
    if (!imgui_->init(glfw_window, std::move(imgui_backend))) {
        GLOG_ERROR("Failed to initialize ImGui");
        render_ctx_->shutdown();
        window_.reset();
        platform::Window::shutdown_sdk();
        return false;
    }

    // 编辑器主题来自持久化偏好；在 ImGuiRenderer 的引擎默认
    // 风格之后覆盖，避免影响示例程序的样式。
    ApplyEditorTheme(settings.theme());
    fluent_theme() = (settings.theme() == EditorTheme::Light)
                         ? FluentTheme::Light() : FluentTheme::Dark();
    apply_editor_font();

    pipeline_ = std::make_unique<render::RenderPipeline>();
    pipeline_->set_viewport_output_enabled(true);
    // 着色器来自引擎自带副本：优先后台解析的引擎默认目录（随 exe 部署的 shaders/
    // 或源码树 src/render/shaders）。旧的 examples/common/shaders 假设已不存在。
    std::filesystem::path shader_dir = render::core_shaders::engine_shaders_dir();
    if (shader_dir.empty()) {
        std::filesystem::path legacy = engine_root / "examples/common/shaders";
        if (std::filesystem::exists(legacy)) {
            shader_dir = legacy;
        }
    }
    if (shader_dir.empty()) {
        GLOG_ERROR("Failed to locate engine default shaders");
        imgui_->shutdown();
        render_ctx_->shutdown();
        window_.reset();
        platform::Window::shutdown_sdk();
        return false;
    }
    if (!pipeline_->init(render_ctx_.get(), shader_dir.string())) {
        GLOG_ERROR("Failed to initialize render pipeline");
        imgui_->shutdown();
        render_ctx_->shutdown();
        window_.reset();
        platform::Window::shutdown_sdk();
        return false;
    }
    if (imgui_ && imgui_->backend()) {
        pipeline_->set_imgui_backend(imgui_->backend());
    }

    world_ = std::make_unique<ecs::World>();
    world_->add_system<ecs::RenderSystem3D>(pipeline_.get());

    // 项目有 scenes/main.gesc 时直接打开，否则新建空场景。
    const std::filesystem::path main_scene =
        project_root / "scenes" / "main.gesc";
    if (std::filesystem::exists(main_scene)) {
        scene_manager_.open_scene(world_.get(), main_scene.string());
        scene_ = world_->scene();
    } else {
        scene_manager_.new_scene(world_.get(), "Untitled");
        scene_ = world_->scene();
        create_default_scene_content(scene_);
    }

    hierarchy_.set_scene(scene_);
    hierarchy_.set_command_stack(&command_stack_);
    inspector_.set_scene(scene_);
    inspector_.set_hierarchy(&hierarchy_);
    inspector_.set_command_stack(&command_stack_);
    file_explorer_.set_root(project_root); // res:// 根 = 项目根

    render_ctx_->start();

    input_ = std::make_unique<platform::InputManager>();
    input_->update(window_.get());

    editor_camera_.set_position(math::Vector3f(5.0f, 4.0f, 5.0f));
    editor_camera_.set_yaw(editor_camera_yaw_);
    editor_camera_.set_pitch(editor_camera_pitch_);

    running_ = true;

    // init 完成：先通知加载子进程退出并等待其结束，再显示主窗口进入主循环。
    if (splash_) splash_->stop();
    window_->set_visible(true);

    GLOG_INFO("Editor initialized successfully");
    return true;
}

void EditorApp::apply_editor_font() {
    if (!imgui_ || !imgui_->backend()) return;

    const std::vector<std::filesystem::path> mono_candidates = {
#ifdef _WIN32
        "C:/Windows/Fonts/CascadiaMono.ttf",
        "C:/Windows/Fonts/consola.ttf",
        "C:/Windows/Fonts/cour.ttf",
#else
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
#endif
        std::filesystem::path(resources::Project::instance().root()) /
            "examples/FPSDemo/fonts/Roboto-Medium.ttf",
    };

    std::filesystem::path font_path;
    for (const auto& candidate : mono_candidates) {
        if (std::filesystem::exists(candidate)) {
            font_path = candidate;
            break;
        }
    }

    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();

    constexpr float k_font_size = 19.0f;
    ImFontConfig mono_cfg;
    mono_cfg.OversampleH = 2;
    mono_cfg.OversampleV = 1;

    if (!font_path.empty()) {
        io.FontDefault = io.Fonts->AddFontFromFileTTF(
            font_path.string().c_str(), k_font_size, &mono_cfg,
            io.Fonts->GetGlyphRangesDefault());
    }
    if (!io.FontDefault) {
        io.FontDefault = io.Fonts->AddFontDefault(&mono_cfg);
    }

    // 等宽字体通常不含 CJK；把系统中文字体合并进去作为回退，保证 UI 中文可读。
    const std::vector<std::filesystem::path> cjk_candidates = {
#ifdef _WIN32
        "C:/Windows/Fonts/msyh.ttc",
        "C:/Windows/Fonts/msyhbd.ttc",
        "C:/Windows/Fonts/simhei.ttf",
#else
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
#endif
    };
for (const auto& candidate : cjk_candidates) {
        if (!std::filesystem::exists(candidate)) continue;
        ImFontConfig cjk_cfg;
        cjk_cfg.MergeMode = true;
        cjk_cfg.OversampleH = 2;
        cjk_cfg.OversampleV = 1;
        io.Fonts->AddFontFromFileTTF(candidate.string().c_str(), k_font_size,
                                     &cjk_cfg,
                                     io.Fonts->GetGlyphRangesChineseFull());
        break;
    }

    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(1.1f);

    // 字体 atlas 在 backend 初始化后重建，需要重新上传到 GPU。
    imgui_->backend()->rebuild_fonts();
}

int EditorApp::run() {
    if (!running_ || !window_) return -1;

    utils::FrameLimiter frame_limiter;
    frame_limiter.set_target_fps(0);

    while (running_ && window_ && !window_->should_close()) {
        frame_limiter.begin_frame();
        window_->poll_events();
        window_->update_frame_stats();
        input_->update(window_.get());

        const float dt =
            std::min(static_cast<float>(window_->delta_time()), 0.1f);

        if (input_->is_key_pressed(GLFW_KEY_ESCAPE)) {
            window_->request_close();
            break;
        }

        handle_tool_shortcuts();

        if (play_mode_.is_playing()) {
            const bool step = play_mode_.is_paused()
                                  ? play_mode_.consume_step_request()
                                  : true;
            if (step) {
                world_->update(dt);
            }
        }

        imgui_->begin_frame();

        // ImGuizmo 每帧需要初始化内部状态（鼠标位置、视口信息等），
        // 必须在任何 Gizmo 操作之前调用。缺少此调用会导致 Gizmo 交互
        // 完全不可用（鼠标事件无法被 ImGuizmo 正确识别）。
        ImGuizmo::BeginFrame();

        // 视口尺寸变化必须先于 UI 渲染处理：resize 会销毁并重建离屏视口纹理，
        // 旧纹理在 ImGui 后端的 descriptor set 随之失效。若等 UI 生成 draw data
        // 之后再重建，渲染线程会绑定一个已释放的 VkDescriptorSet（Vulkan 校验
        // 层报 invalid object，驱动直接崩溃）。提前处理保证本帧 UI 采样的是新纹理。
        if (viewport_resize_pending_) {
            resize_viewport_if_needed(static_cast<int>(viewport_size_.x),
                                      static_cast<int>(viewport_size_.y));
        }
        render_editor_ui();

        int window_width = 0;
        int window_height = 0;
        window_->get_size(window_width, window_height);
        render_ctx_->set_viewport(0, 0, window_width, window_height);

        const int viewport_width = viewport_size_valid_
                                       ? static_cast<int>(viewport_size_.x)
                                       : window_width;
        const int viewport_height = viewport_size_valid_
                                        ? static_cast<int>(viewport_size_.y)
                                        : window_height;
        editor_camera_.set_aspect(
            viewport_height > 0
                ? static_cast<float>(viewport_width) /
                      static_cast<float>(viewport_height)
                : 16.0f / 9.0f);

        collect_scene_lights();
        pipeline_->set_camera(editor_camera_);
        pipeline_->set_lights(editor_lights_);
        pipeline_->set_viewport(viewport_width, viewport_height);
        // 应用渲染效果
        pipeline_->set_ssr_enabled(ssr_enabled_);
        pipeline_->set_dof_enabled(dof_enabled_);
        pipeline_->set_dof_params(dof_focus_distance_, dof_focus_range_, dof_blur_amount_);
        pipeline_->set_motion_blur_enabled(motion_blur_enabled_);
        pipeline_->set_motion_blur_amount(motion_blur_amount_);
        pipeline_->set_fog_enabled(fog_enabled_);
        pipeline_->set_fog_params(fog_color_, fog_density_, fog_height_);
        pipeline_->set_water_enabled(water_enabled_);
        pipeline_->set_point_shadow_enabled(point_shadow_enabled_);
        pipeline_->set_shadow_atlas_enabled(shadow_atlas_enabled_);
        pipeline_->set_shadow_mode(static_cast<render::RenderPipeline::ShadowMode>(shadow_mode_));
        pipeline_->set_gi_mode(static_cast<render::GIMode>(gi_mode_));
        pipeline_->set_gi_indirect_intensity(gi_indirect_intensity_);
        world_->render(*render_ctx_);

        // RenderPipeline 在 viewport_output 模式下会停在离屏 FBO；
        // 必须显式恢复默认 framebuffer（OpenGL 直接绑定 0，Vulkan 由
        // unbind_framebuffer 重新开启 swapchain render pass），
        // 否则 ImGui 会画进 Viewport 纹理，主窗口表现为全黑。
        render_ctx_->set_framebuffer(render::RHIFramebufferHandle{});

        // ImGui 绘制命令必须在场景命令之后提交，否则 UI 会被场景渲染覆盖。
        imgui_->end_frame(
            [this](ImDrawData* draw_data,
                   std::shared_ptr<std::promise<void>> sync_promise) {
                auto owned_draw_data = imgui_->clone_draw_data(draw_data);
                render_ctx_->push_command(
                    [owned_draw_data, this,
                     sync_promise](render::IRenderBackend*) {
                        imgui_->render_draw_data(owned_draw_data.get());
                        sync_promise->set_value();
                    });
            });

        render_ctx_->present();
        apply_deferred_deletes();

        frame_limiter.end_frame();
    }

    return 0;
}

void EditorApp::shutdown() {
    if (!window_) return;
    running_ = false;

    if (render_ctx_ && render_ctx_->is_running()) {
        render_ctx_->pause_render_thread_keep_cmdbuffer();
    }

    if (world_) {
        scene_manager_.detach_active_from_world(world_.get());
        world_->shutdown();
    }
    scene_ = nullptr;

    if (imgui_) {
        imgui_->shutdown();
    }
    if (pipeline_) {
        pipeline_->shutdown();
    }
    if (render_ctx_) {
        render_ctx_->shutdown();
    }

    // 先记住窗口尺寸与最大化状态，再释放资源。
    {
        EditorSettings& s = EditorSettings::instance();
        if (window_) {
            int w = 0, h = 0;
            window_->get_size(w, h);
            s.set_window_width(w);
            s.set_window_height(h);
            s.set_window_maximized(window_->is_maximized());
        }
        s.save();
    }

    window_.reset();
    platform::Window::shutdown_sdk();
    GLOG_INFO("Editor shutdown complete");
}

void EditorApp::init_docking_layout(bool force) {
    static bool initialized_once = false;
    if (initialized_once && !force) return;
    initialized_once = true;

    ImGuiID dockspace_id = ImGui::GetID("DockSpace");
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);

    ImGuiID left_id = 0;
    ImGuiID left_top_id = 0;
    ImGuiID left_bot_id = 0;
    ImGuiID right_id = 0;
    ImGuiID bottom_id = 0;
    ImGuiID center_id = 0;

    ImGui::DockBuilderSetNodeSize(dockspace_id,
                                  ImGui::GetContentRegionAvail());

    left_id = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Left, 0.20f,
                                          nullptr, &center_id);
    // Inspector 面板加宽（28%），为颜色选择器等编辑控件留出空间
    right_id = ImGui::DockBuilderSplitNode(center_id, ImGuiDir_Right, 0.28f,
                                           nullptr, &center_id);
    bottom_id = ImGui::DockBuilderSplitNode(center_id, ImGuiDir_Down, 0.25f,
                                            nullptr, &center_id);

    // 左侧再垂直拆分：上 Scene（60%），下 FileSystem（40%）
    ImGui::DockBuilderSplitNode(left_id, ImGuiDir_Down, 0.40f,
                                &left_bot_id, &left_top_id);

    ImGui::DockBuilderDockWindow("Scene", left_top_id);
    ImGui::DockBuilderDockWindow("FileSystem", left_bot_id);
    ImGui::DockBuilderDockWindow("Inspector", right_id);
    ImGui::DockBuilderDockWindow("Viewport", center_id);
    ImGui::DockBuilderDockWindow("Output", bottom_id);
    ImGui::DockBuilderFinish(dockspace_id);
}

void EditorApp::render_editor_ui() {
    handle_shortcuts();
    scene_ = world_->scene();
    if (scene_ && scene_->has_unsaved_changes()) {
        scene_manager_.mark_active_dirty();
    }

    // 菜单栏固定在窗口顶部。
    render_menu_bar();

    // 工具栏（紧贴菜单栏下方）
    render_toolbar();

    // Scene Tab Bar（顶部独立栏，工具栏下面）
    render_scene_tab_bar();

    // 只有 Viewport / Hierarchy / Inspector / Console / Project 进入 docking。
    auto* viewport_p =
        reinterpret_cast<ImGuiViewportP*>(ImGui::GetMainViewport());
    const ImRect work_rect = viewport_p->GetBuildWorkRect();

    ImGui::SetNextWindowPos(work_rect.Min, ImGuiCond_Always);
    ImGui::SetNextWindowSize(work_rect.GetSize(), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("MainDockSpace", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoDocking |
                     ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoNav);

    const ImGuiID dockspace_id = ImGui::GetID("DockSpace");
    init_docking_layout();
    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f),
                     ImGuiDockNodeFlags_None);
    ImGui::End();
    ImGui::PopStyleVar();

    render_viewport();
    render_hierarchy();
    render_inspector();
    render_console();
    if (show_render_settings_) {
        render_settings_panel();
    }
    file_explorer_.render_as_tab(true);

    if (show_editor_settings_) {
        render_editor_settings_panel();
    }

    if (show_demo_window_) {
        ImGui::ShowDemoWindow(&show_demo_window_);
    }
    if (show_metrics_window_) {
        ImGui::ShowMetricsWindow(&show_metrics_window_);
    }

    render_status_bar();
}

void EditorApp::render_toolbar() {
    const float tb_h = ImGui::GetFrameHeight() + 8.0f;
    if (ImGui::BeginViewportSideBar(
            "##ToolBar", ImGui::GetMainViewport(), ImGuiDir_Up, tb_h,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings)) {
        // Fluent 风格：主操作使用强调色，其余为普通按钮。
        const FluentTheme& theme = fluent_theme();
        {
            FluentButton new_btn(/*accent=*/false);
            if (new_btn.Draw(tr("New Scene"), theme)) {
                if (play_mode_.is_playing()) stop_play();
                pause_scene_transition();
                scene_manager_.new_scene(world_.get(), "Untitled");
                scene_ = world_->scene();
                create_default_scene_content(scene_);
                resume_scene_transition();
                hierarchy_.set_scene(scene_);
                inspector_.set_scene(scene_);
            }
        }
        ImGui::SameLine();
        {
            FluentButton open_btn;
            if (open_btn.Draw(tr("Open Scene"), theme)) {
                if (play_mode_.is_playing()) stop_play();
                open_scene_dialog();
            }
        }
        ImGui::SameLine();
        {
            FluentButton save_btn;
            if (save_btn.Draw(tr("Save Scene"), theme)) {
                if (scene_manager_.active_path().empty()) {
                    save_scene_dialog();
                } else {
                    scene_manager_.save_active();
                }
            }
        }

        ImGui::SameLine();
        ImGui::Text("|");
        ImGui::SameLine();

        // 播放控制
        if (!play_mode_.is_playing()) {
            FluentButton play_btn(/*accent=*/true);
            if (play_btn.Draw(tr("Play"), theme)) {
                play_mode_.begin_play(scene_);
            }
        } else {
            FluentButton stop_btn(/*accent=*/true);
            if (stop_btn.Draw(tr("Stop"), theme)) {
                stop_play();
            }
            ImGui::SameLine();
            const bool paused = play_mode_.is_paused();
            FluentButton pause_btn;
            if (pause_btn.Draw(tr(paused ? "Resume" : "Pause"), theme)) {
                play_mode_.toggle_pause();
            }
            if (paused) {
                ImGui::SameLine();
                FluentButton step_btn;
                if (step_btn.Draw(tr("Step"), theme)) {
                    play_mode_.request_step();
                }
            }
        }

        ImGui::SameLine();
        ImGui::Text("|");
        ImGui::SameLine();
        {
            FluentButton settings_btn;
            if (settings_btn.Draw(tr("Editor Settings..."), theme)) {
                show_editor_settings_ = true;
            }
        }
        ImGui::End();
    }
}

// 递归统计场景实体总数（含根节点）。
namespace {
int count_entities(scene::Entity* node) {
    if (!node) return 0;
    int count = 1;
    for (auto& child : node->children()) {
        count += count_entities(child.get());
    }
    return count;
}
} // namespace

void EditorApp::render_status_bar() {
    const float sb_h = ImGui::GetFrameHeight() + 8.0f;
    if (ImGui::BeginViewportSideBar(
            "##StatusBar", ImGui::GetMainViewport(), ImGuiDir_Down, sb_h,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings)) {
        const float fps = ImGui::GetIO().Framerate;
        const float ms = window_ ? window_->delta_time() * 1000.0f : 0.0f;
        const int entities =
            scene_ ? count_entities(scene_->root()) : 0;

        char buf[160];
        snprintf(buf, sizeof(buf), "FPS: %.1f   |   %s: %.2f ms   |   %s: %d",
                 fps, tr("Frame"), ms, tr("Entities"), entities);
        ImGui::TextUnformatted(buf);

        // 右侧显示当前语言
        ImGui::SameLine(ImGui::GetWindowWidth() - 72.0f);
        ImGui::TextUnformatted(
            I18n::instance().language() == "zh" ? "中文" : "English");
        ImGui::End();
    }
}

void EditorApp::render_menu_bar() {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu(tr("File"))) {
            if (ImGui::MenuItem(tr("New Scene"), "Ctrl+N")) {
                if (play_mode_.is_playing()) stop_play();
                pause_scene_transition();
                scene_manager_.new_scene(world_.get(), "Untitled");
                scene_ = world_->scene();
                create_default_scene_content(scene_);
                resume_scene_transition();
                hierarchy_.set_scene(scene_);
                inspector_.set_scene(scene_);
            }
            if (ImGui::MenuItem(tr("Open Scene"), "Ctrl+O")) {
                if (play_mode_.is_playing()) stop_play();
                open_scene_dialog();
            }
            ImGui::Separator();
            if (ImGui::MenuItem(tr("Save Scene"), "Ctrl+S")) {
                if (scene_manager_.active_path().empty()) {
                    save_scene_dialog();
                } else {
                    scene_manager_.save_active();
                }
            }
            if (ImGui::MenuItem(tr("Save As..."), "Ctrl+Shift+S")) {
                save_scene_dialog();
            }
            ImGui::Separator();
            if (ImGui::MenuItem(tr("Close Scene"))) {
                if (scene_manager_.has_active()) {
                    pause_scene_transition();
                    scene_manager_.close_scene(scene_manager_.active_index(),
                                               world_.get());
                    resume_scene_transition();
                    scene_ = world_->scene();
                    hierarchy_.set_scene(scene_);
                    inspector_.set_scene(scene_);
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem(tr("Exit"))) {
                window_->request_close();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(tr("Edit"))) {
            if (ImGui::MenuItem(tr("Undo"), "Ctrl+Z",
                                false, command_stack_.can_undo())) {
                command_stack_.undo();
            }
            if (ImGui::MenuItem(tr("Redo"), "Ctrl+Y",
                                false, command_stack_.can_redo())) {
                command_stack_.redo();
            }
            ImGui::Separator();
            if (ImGui::MenuItem(tr("Editor Settings..."))) {
                show_editor_settings_ = true;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(tr("View"))) {
            ImGui::MenuItem(tr("Demo Window"), nullptr, &show_demo_window_);
            ImGui::MenuItem(tr("Metrics"), nullptr, &show_metrics_window_);
            ImGui::Separator();
            if (ImGui::MenuItem(tr("Default Layout"))) {
                init_docking_layout(true);
            }
            ImGui::Separator();
            ImGui::MenuItem(tr("Render Settings"), nullptr,
                            &show_render_settings_);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(tr("Help"))) {
            if (ImGui::MenuItem(tr("About Gryce Engine"))) {
                GLOG_INFO("Gryce Engine ImGui Editor");
            }
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
}

void EditorApp::render_settings_panel() {
    ImGui::SetNextWindowSize(ImVec2(400, 500), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(tr("Render Settings"), &show_render_settings_)) {
        ImGui::End();
        return;
    }

    // ---- Shadow ----
    ImGui::SeparatorText(tr("Shadow"));
    FluentToggle point_shadow(&point_shadow_enabled_);
    point_shadow.Draw(tr("Point Shadow"), fluent_theme());
    const char* k_shadow_modes[] = { "PCF", "VSM", "ESM" };
    FluentCombo shadow_combo(k_shadow_modes, 3, &shadow_mode_);
    shadow_combo.Draw(tr("Shadow Mode"), fluent_theme());
    FluentToggle shadow_atlas(&shadow_atlas_enabled_);
    shadow_atlas.Draw(tr("Shadow Atlas"), fluent_theme());

    // ---- Post-Processing ----
    ImGui::SeparatorText(tr("Post-Processing"));
    FluentToggle ssr_toggle(&ssr_enabled_);
    ssr_toggle.Draw(tr("SSR"), fluent_theme());
    FluentToggle dof_toggle(&dof_enabled_);
    dof_toggle.Draw(tr("DOF"), fluent_theme());
    if (dof_enabled_) {
        ImGui::Indent();
        FluentSlider dof_focus_d(&dof_focus_distance_, 1.0f, 100.0f, "%.2f");
        dof_focus_d.Draw(tr("Focus Distance"), fluent_theme());
        FluentSlider dof_focus_r(&dof_focus_range_, 1.0f, 50.0f, "%.2f");
        dof_focus_r.Draw(tr("Focus Range"), fluent_theme());
        FluentSlider dof_blur(&dof_blur_amount_, 1.0f, 20.0f, "%.2f");
        dof_blur.Draw(tr("Blur Amount"), fluent_theme());
        ImGui::Unindent();
    }
    FluentToggle motion_blur(&motion_blur_enabled_);
    motion_blur.Draw(tr("Motion Blur"), fluent_theme());
    if (motion_blur_enabled_) {
        ImGui::Indent();
        FluentSlider motion_blur_amount(&motion_blur_amount_, 0.1f, 2.0f, "%.2f");
        motion_blur_amount.Draw(tr("Blur Amount"), fluent_theme());
        ImGui::Unindent();
    }

    // ---- Environment ----
    ImGui::SeparatorText(tr("Environment"));
    FluentToggle fog_toggle(&fog_enabled_);
    fog_toggle.Draw(tr("Volumetric Fog"), fluent_theme());
    if (fog_enabled_) {
        ImGui::Indent();
        ImGui::ColorEdit3(tr("Fog Color"), &fog_color_.x);
        FluentSlider fog_density(&fog_density_, 0.001f, 0.1f, "%.3f");
        fog_density.Draw(tr("Density"), fluent_theme());
        FluentSlider fog_height(&fog_height_, 1.0f, 50.0f, "%.2f");
        fog_height.Draw(tr("Height"), fluent_theme());
        ImGui::Unindent();
    }
    FluentToggle water_toggle(&water_enabled_);
    water_toggle.Draw(tr("Water"), fluent_theme());

    // ---- GI ----
    ImGui::SeparatorText(tr("Global Illumination"));
    const char* k_gi_modes[] = { "None", "SDFGI", "VoxelGI" };
    FluentCombo gi_combo(k_gi_modes, 3, &gi_mode_);
    gi_combo.Draw(tr("GI Mode"), fluent_theme());
    FluentSlider gi_intensity(&gi_indirect_intensity_, 0.0f, 5.0f, "%.2f");
    gi_intensity.Draw(tr("Indirect Intensity"), fluent_theme());

    ImGui::End();
}

void EditorApp::render_editor_settings_panel() {
    EditorSettings& s = EditorSettings::instance();
    ImGui::SetNextWindowSize(ImVec2(420, 330), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(tr("Editor Settings"), &show_editor_settings_)) {
        ImGui::End();
        return;
    }

    // ---- 外观：主题切换（即时生效并持久化）----
    ImGui::SeparatorText(tr("Appearance"));
    const char* k_themes[] = {
        EditorThemeName(EditorTheme::Dark),
        EditorThemeName(EditorTheme::Light),
    };
    int cur = static_cast<int>(s.theme());
    FluentCombo theme_combo(k_themes, 2, &cur);
    if (theme_combo.Draw(tr("Theme"), fluent_theme())) {
        const auto theme = static_cast<EditorTheme>(cur);
        s.set_theme(theme);
        ApplyEditorTheme(theme);
        fluent_theme() = (theme == EditorTheme::Light)
                            ? FluentTheme::Light() : FluentTheme::Dark();
    }

    // ---- 语言切换（i18n，即时生效）----
    ImGui::SeparatorText(tr("Language"));
    static int lang_idx = (s.language() == "en") ? 1 : 0;
    const char* k_langs[] = {"中文", "English"};
    FluentCombo lang_combo(k_langs, 2, &lang_idx);
    if (lang_combo.Draw(tr("Language"), fluent_theme())) {
        const std::string new_lang = lang_idx == 0 ? "zh" : "en";
        s.set_language(new_lang);
        I18n::instance().set_language(new_lang);
    }

    // ---- 窗口尺寸 ----
    ImGui::SeparatorText(tr("Window"));
    static int win_w = 0, win_h = 0;
    static bool win_init = false;
    if (!win_init) {
        if (window_) {
            window_->get_size(win_w, win_h);
        }
        win_init = true;
    }
    ImGui::DragInt(tr("Width"), &win_w, 1.0f, 320, 8192);
    ImGui::DragInt(tr("Height"), &win_h, 1.0f, 240, 8192);
    bool apply = false;
    {
        FluentButton apply_btn(true);
        apply = apply_btn.Draw(tr("Apply Size"), fluent_theme());
    }
    if (apply && window_) {
        window_->set_size(win_w, win_h);
        s.set_window_width(win_w);
        s.set_window_height(win_h);
    }
    ImGui::TextDisabled("%s", tr("Maximize state is saved automatically on close"));

    ImGui::End();
}

void EditorApp::render_viewport() {
    ImGui::Begin("Viewport");
    viewport_hovered_ = ImGui::IsWindowHovered();

    // =====================================================
    // 左侧：Viewport 变换工具条（Select/Move/Rotate/Scale）
    // =====================================================
    const float tool_bar_width = 40.0f;
    {
        const float content_height = ImGui::GetContentRegionAvail().y;
        ImGui::BeginChild("##vp_tools", ImVec2(tool_bar_width, content_height),
                          false, ImGuiWindowFlags_NoScrollbar);
        render_viewport_tool_buttons();
        ImGui::EndChild();
    }

    ImGui::SameLine();

    // --- 视口内容区域 ---
    const ImVec2 available = ImGui::GetContentRegionAvail();
    if (available.x > 1.0f && available.y > 1.0f) {
        if (!viewport_size_valid_ ||
            std::fabs(available.x - viewport_size_.x) > 0.5f ||
            std::fabs(available.y - viewport_size_.y) > 0.5f) {
            viewport_size_ = available;
            viewport_size_valid_ = true;
            viewport_resize_pending_ = true;
        }
    }

    bool viewport_image_hovered = false;
    const uint64_t texture_id = viewport_texture_id();
    if (texture_id != 0) {
        ImTextureID tex = (ImTextureID)(uintptr_t)texture_id;
        if (vulkan_) {
            ImGui::Image(tex, available);
        } else {
            // OpenGL FBO 纹理在 ImGui 中通常需要上下翻转 UV。
            ImGui::Image(tex, available,
                         ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
        }
        viewport_image_hovered = ImGui::IsItemHovered();

        // 在视口图像上叠加 Gizmo
        render_gizmo();

        // 处理视口点击选择
        handle_viewport_click_to_select();
    } else {
        ImGui::TextUnformatted("Viewport rendering initializing...");
    }

    // --- 播放按钮叠加（视口右上角） ---
    {
        const ImVec2 item_min = ImGui::GetItemRectMin();
        const ImVec2 item_max = ImGui::GetItemRectMax();
        const float btn_w = 34.0f;
        const float btn_h = 28.0f;
        ImGui::SetCursorScreenPos(ImVec2(
            item_max.x - btn_w - 8.0f, item_min.y + 8.0f));

        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImVec4(0.0f, 0.0f, 0.0f, 0.55f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(0.0f, 0.0f, 0.0f, 0.75f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                              ImVec4(0.30f, 0.60f, 1.00f, 0.50f));

        if (!play_mode_.is_playing()) {
            if (ImGui::Button("\xe2\x96\xb6", ImVec2(btn_w, btn_h))) {
                start_play();
            }
        } else {
            if (ImGui::Button("\xe2\x96\xa0", ImVec2(btn_w, btn_h))) {
                stop_play();
            }
        }

        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();
    }

    // 修复：使用视口图像的 hover 状态来控制相机，而不是错误地使用播放按钮
    viewport_hovered_ = viewport_image_hovered || ImGui::IsWindowHovered();
    if (viewport_hovered_) {
        update_editor_camera(0.016f);
    }

    ImGui::End();
}

void EditorApp::render_viewport_tool_buttons() {
    constexpr float k_tool_bar_width = 40.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 4));
    ImGui::Dummy(ImVec2(0, 6)); // 顶部间距

    // Godot 风格图标：Select / Move / Rotate / Scale
    struct ToolInfo {
        const char* icon;
        const char* tip;
        ViewportTool tool;
    };
    const ToolInfo tools[] = {
        {"S", "Select (Q)",    ViewportTool::Select},
        {"M", "Move (W)",      ViewportTool::Move},
        {"R", "Rotate (E)",    ViewportTool::Rotate},
        {"Sc","Scale (R)",     ViewportTool::Scale},
    };

    const float btn_size = k_tool_bar_width - 8;
    const FluentTheme& theme = fluent_theme();
    for (int i = 0; i < 4; ++i) {
        const bool active = (current_tool_ == tools[i].tool);

        // 当前工具用强调色高亮，其余为透明普通按钮
        FluentButton tool_btn(active);
        tool_btn.width  = btn_size;
        tool_btn.height = btn_size;
        if (tool_btn.Draw(tools[i].icon, theme)) {
            current_tool_ = tools[i].tool;
        }

        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", tools[i].tip);
        }
    }

    ImGui::PopStyleVar(1);
}

// ---------------------------------------------------------------------------
// 视口 Gizmo 渲染
// ---------------------------------------------------------------------------
void EditorApp::render_gizmo() {
    if (!scene_) return;

    scene::Entity* selected = hierarchy_.selected_entity();
    if (!selected) return;

    auto* transform = selected->get_component<components::Transform>();
    if (!transform) return;

    // 获取相机矩阵
    math::Matrix4f view = editor_camera_.get_view_matrix();
    math::Matrix4f proj = editor_camera_.get_projection_matrix();

    float view_mat[16], proj_mat[16], model_mat[16];
    std::memcpy(view_mat, view.m, sizeof(float) * 16);
    std::memcpy(proj_mat, proj.m, sizeof(float) * 16);

    // 获取实体局部矩阵
    math::Matrix4f model = transform->local_matrix();
    std::memcpy(model_mat, model.m, sizeof(float) * 16);

    // 设置 Gizmo 操作 — 根据当前工具决定
    ImGuizmo::OPERATION op = ImGuizmo::TRANSLATE;
    switch (current_tool_) {
        case ViewportTool::Move:   op = ImGuizmo::TRANSLATE; break;
        case ViewportTool::Rotate: op = ImGuizmo::ROTATE;    break;
        case ViewportTool::Scale:  op = ImGuizmo::SCALE;     break;
        default:                   op = ImGuizmo::TRANSLATE; // Select 模式也默认显示移动 Gizmo
    }

    // 设置 Gizmo 矩形区域 = 视口内容区域
    const ImVec2 vp_min = ImGui::GetItemRectMin();
    const ImVec2 vp_max = ImGui::GetItemRectMax();
    ImGuizmo::SetRect(vp_min.x, vp_min.y, vp_max.x - vp_min.x, vp_max.y - vp_min.y);
    ImGuizmo::SetDrawlist();

    ImGuizmo::MODE mode = ImGuizmo::WORLD;
    if (op == ImGuizmo::SCALE) mode = ImGuizmo::LOCAL;

    // 操作 Gizmo — 选中物体后始终可拖拽交互
    if (ImGuizmo::Manipulate(view_mat, proj_mat, op, mode, model_mat)) {
        // 将 Gizmo 修改后的矩阵分解回 Transform
        float translation[3], rotation[3], scale[3];
        ImGuizmo::DecomposeMatrixToComponents(model_mat, translation, rotation, scale);

        transform->position = math::Vector3f(translation[0], translation[1], translation[2]);
        transform->rotation = math::Quaternionf::from_euler(rotation[0], rotation[1], rotation[2]);
        transform->scale = math::Vector3f(scale[0], scale[1], scale[2]);
    }
}

// ---------------------------------------------------------------------------
// 视口点击选择实体（射线检测）
// ---------------------------------------------------------------------------
void EditorApp::handle_viewport_click_to_select() {
    if (!scene_ || !viewport_hovered_) return;
    // 如果用户正在拖拽 Gizmo，不触发选择
    if (ImGuizmo::IsUsing()) return;
    // 鼠标悬停在 Gizmo 上时也不触发选择
    if (ImGuizmo::IsOver()) return;

    ImGuiIO& io = ImGui::GetIO();
    // 只有纯粹的点击才触发选择，拖拽时不触发
    if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return;
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) return;

    // 获取鼠标在视口中的位置
    const ImVec2 vp_min = ImGui::GetItemRectMin();
    const ImVec2 vp_max = ImGui::GetItemRectMax();
    const ImVec2 vp_size(vp_max.x - vp_min.x, vp_max.y - vp_min.y);

    const float mx = io.MousePos.x - vp_min.x;
    const float my = io.MousePos.y - vp_min.y;

    if (mx < 0 || my < 0 || mx > vp_size.x || my > vp_size.y) return;

    // 归一化设备坐标 [-1, 1]
    const float ndc_x = (mx / vp_size.x) * 2.0f - 1.0f;
    const float ndc_y = 1.0f - (my / vp_size.y) * 2.0f;

    // 从相机发射射线
    const math::Matrix4f inv_vp = (editor_camera_.get_projection_matrix() *
                                   editor_camera_.get_view_matrix()).inverse();

    // 近平面和远平面上的点
    math::Vector4f near_pt = inv_vp * math::Vector4f(ndc_x, ndc_y, 0.0f, 1.0f);
    math::Vector4f far_pt  = inv_vp * math::Vector4f(ndc_x, ndc_y, 1.0f, 1.0f);
    near_pt = near_pt / near_pt.w;
    far_pt  = far_pt / far_pt.w;

    const math::Vector3f ray_origin(near_pt.x, near_pt.y, near_pt.z);
    const math::Vector3f ray_dir = (math::Vector3f(far_pt.x, far_pt.y, far_pt.z) - ray_origin).normalized();

    // 遍历所有 MeshRenderer 实体，检测射线相交
    scene::Entity* hit_entity = nullptr;
    float hit_dist = std::numeric_limits<float>::max();

    scene_->foreach([&](scene::Entity* entity) {
        auto* mr = entity->get_component<components::MeshRenderer>();
        auto* tr = entity->get_component<components::Transform>();
        if (!mr || !tr || !mr->enabled) return;

        // 计算世界空间中的包围球
        const math::Vector3f world_pos = entity->world_transform().translation();
        const float sphere_radius = 1.0f; // 默认半径

        // 射线与球体相交检测
        const math::Vector3f oc = ray_origin - world_pos;
        const float a = ray_dir.dot(ray_dir);
        const float b = 2.0f * oc.dot(ray_dir);
        const float c = oc.dot(oc) - sphere_radius * sphere_radius;
        const float d = b * b - 4.0f * a * c;

        if (d >= 0.0f) {
            const float t = (-b - std::sqrt(d)) / (2.0f * a);
            if (t > 0.0f && t < hit_dist) {
                hit_dist = t;
                hit_entity = entity;
            }
        }
    });

    if (hit_entity) {
        hierarchy_.set_selected_uuid(hit_entity->uuid());
    }
}

void EditorApp::render_hierarchy() {
    hierarchy_.render();
}

void EditorApp::render_inspector() {
    inspector_.set_play_mode(play_mode_.is_playing());
    inspector_.render();
}

void EditorApp::render_console() {
    console_.render();
}

void EditorApp::render_scene_tab_bar() {
    // Blender 顶部栏风格：比默认更高、与菜单栏同底色、标签更宽松。
    const float tab_bar_height = ImGui::GetFrameHeight() * 1.45f + 6.0f;
    ImGui::PushStyleColor(ImGuiCol_WindowBg,
                          ImGui::GetStyleColorVec4(ImGuiCol_MenuBarBg));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14.0f, 10.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_TabBorderSize, 0.0f);

    if (ImGui::BeginViewportSideBar(
            "##SceneTabsBar", ImGui::GetMainViewport(), ImGuiDir_Up,
            tab_bar_height,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings)) {
        if (play_mode_.is_playing()) {
            ImGui::TextDisabled(tr("Play mode active"));
            ImGui::End();
        } else {
            int close_pending = -1;
            ImGuiTabBarFlags tab_flags = ImGuiTabBarFlags_AutoSelectNewTabs |
                                         ImGuiTabBarFlags_Reorderable |
                                         ImGuiTabBarFlags_FittingPolicyScroll;
            if (ImGui::BeginTabBar("scene_tabs", tab_flags)) {
                const auto& scenes = scene_manager_.scenes();
                for (int i = 0; i < static_cast<int>(scenes.size()); ++i) {
                    const bool is_active = (i == scene_manager_.active_index());
                    std::string title = scenes[i].title.empty()
                                            ? "Untitled"
                                            : scenes[i].title;
                    if (scenes[i].dirty) {
                        title += " *";
                    }
                    title += "##scene_" + std::to_string(i);

                    bool open = true;
                    const ImGuiTabItemFlags flags =
                        is_active ? ImGuiTabItemFlags_SetSelected
                                  : ImGuiTabItemFlags_None;
                    if (ImGui::BeginTabItem(title.c_str(), &open, flags)) {
                        if (!is_active) {
                            pause_scene_transition();
                            const bool activated =
                                scene_manager_.activate_scene(i, world_.get());
                            resume_scene_transition();
                            if (!activated) {
                                ImGui::EndTabItem();
                                continue;
                            }
                            scene_ = world_->scene();
                            hierarchy_.set_scene(scene_);
                            inspector_.set_scene(scene_);
                        }
                        ImGui::EndTabItem();
                    }
                    if (!open) {
                        close_pending = i;
                    }
                }
                ImGui::SameLine(0.0f, 0.0f);
                if (ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing |
                                                  ImGuiTabItemFlags_NoTooltip)) {
                    pause_scene_transition();
                    scene_manager_.new_scene(world_.get(), "Untitled");
                    scene_ = world_->scene();
                    create_default_scene_content(scene_);
                    resume_scene_transition();
                    hierarchy_.set_scene(scene_);
                    inspector_.set_scene(scene_);
                }
                ImGui::EndTabBar();
            }
            if (close_pending >= 0) {
                pause_scene_transition();
                scene_manager_.close_scene(close_pending, world_.get());
                resume_scene_transition();
                scene_ = world_->scene();
                hierarchy_.set_scene(scene_);
                inspector_.set_scene(scene_);
            }
            ImGui::End();
        }
    }
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor();
}

void EditorApp::handle_shortcuts() {
    if (!ImGui::GetIO().WantTextInput) {
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z)) {
            command_stack_.undo();
        }
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y)) {
            command_stack_.redo();
        }
    }

    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_N)) {
        if (play_mode_.is_playing()) stop_play();
        pause_scene_transition();
        scene_manager_.new_scene(world_.get(), "Untitled");
        scene_ = world_->scene();
        create_default_scene_content(scene_);
        resume_scene_transition();
        hierarchy_.set_scene(scene_);
        inspector_.set_scene(scene_);
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O)) {
        if (play_mode_.is_playing()) stop_play();
        open_scene_dialog();
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S)) {
        save_scene_dialog();
    } else if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S)) {
        if (scene_manager_.active_path().empty()) {
            save_scene_dialog();
        } else {
            scene_manager_.save_active();
        }
    }
}

void EditorApp::create_default_scene_content(scene::Scene* scene) {
    if (!scene) return;

    // 环境光（方向光）
    scene::Entity* light_entity = scene->create_entity("DirectionalLight");
    auto* light = light_entity->add_component<components::Light>();
    light->light_type = components::Light::Type::Directional;
    light->intensity = 2.0f;
    light->direction = math::Vector3f(-0.4f, -1.0f, -0.3f).normalized();

    // 设置全局环境光颜色
    if (pipeline_) {
        pipeline_->set_ambient(math::Vector3f(0.2f, 0.2f, 0.25f));
    }

    // 主摄像机
    scene::Entity* camera_entity = scene->create_entity("Main Camera");
    camera_entity->add_component<components::Camera>();
    auto* camera_transform = camera_entity->transform();
    camera_transform->position = math::Vector3f(0.0f, 2.0f, 5.0f);
    camera_transform->rotation = math::Quaternionf::from_euler(-20.0f, 0.0f, 0.0f);
}

void EditorApp::open_scene_dialog() {
    const std::string path = native_file_dialog(false, "Open Gryce Scene");
    if (path.empty()) return;

    pause_scene_transition();
    const bool opened = scene_manager_.open_scene(world_.get(), path);
    resume_scene_transition();
    if (!opened) {
        GLOG_ERROR("Failed to open scene '{}'", path);
        return;
    }
    scene_ = world_->scene();
    hierarchy_.set_scene(scene_);
    inspector_.set_scene(scene_);
    GLOG_INFO("Opened scene '{}'", path);
}

void EditorApp::save_scene_dialog() {
    const std::string path = native_file_dialog(true, "Save Gryce Scene");
    if (path.empty()) return;

    if (!scene_manager_.save_active(path)) {
        GLOG_ERROR("Failed to save scene to '{}'", path);
        return;
    }
    GLOG_INFO("Saved scene '{}'", path);
}

void EditorApp::start_play() {
    scene_ = world_->scene();
    if (!scene_) return;
    play_mode_.begin_play(scene_);
    world_->set_updates_enabled(true);
    GLOG_INFO("Entering play mode");
}

void EditorApp::stop_play() {
    if (!play_mode_.is_playing()) return;
    auto restored = play_mode_.end_play();
    if (restored) {
        pause_scene_transition();
        scene_manager_.replace_active(world_.get(), std::move(restored));
        resume_scene_transition();
    }
    world_->set_updates_enabled(false);
    scene_ = world_->scene();
    hierarchy_.set_scene(scene_);
    inspector_.set_scene(scene_);
    GLOG_INFO("Exiting play mode");
}

void EditorApp::pause_scene_transition() {
    command_stack_.clear();
    if (render_ctx_ && render_ctx_->is_running()) {
        render_ctx_->pause_render_thread_keep_cmdbuffer();
    }
}

void EditorApp::resume_scene_transition() {
    if (render_ctx_ && !render_ctx_->is_running()) {
        render_ctx_->resume_render_thread();
    }
}

// ---------------------------------------------------------------------------
// 工具快捷键
// ---------------------------------------------------------------------------
void EditorApp::handle_tool_shortcuts() {
    if (ImGui::GetIO().WantTextInput) return;

    if (input_->is_key_pressed(GLFW_KEY_Q)) {
        current_tool_ = ViewportTool::Select;
    } else if (input_->is_key_pressed(GLFW_KEY_W)) {
        current_tool_ = ViewportTool::Move;
    } else if (input_->is_key_pressed(GLFW_KEY_E)) {
        current_tool_ = ViewportTool::Rotate;
    } else if (input_->is_key_pressed(GLFW_KEY_R)) {
        current_tool_ = ViewportTool::Scale;
    }
}

void EditorApp::update_editor_camera(float dt) {
    if (!viewport_hovered_) return;

    ImGuiIO& io = ImGui::GetIO();

    // 左键拖拽 → 旋转视角（仅当 Gizmo 未被主动拖拽时触发）
    // 使用 IsUsing() 而非 IsOver()：IsOver() 在鼠标悬停时即返回 true，
    // 旋转 Gizmo 的命中区域较大，会导致普通视角旋转被误拦截；
    // IsUsing() 仅在用户正在拖拽 Gizmo 时返回 true，更精确。
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Left) && !ImGuizmo::IsUsing()) {
        editor_camera_yaw_ += io.MouseDelta.x * 0.35f;
        editor_camera_pitch_ =
            std::clamp(editor_camera_pitch_ - io.MouseDelta.y * 0.35f, -89.0f,
                       89.0f);
    }

    // 右键拖拽 → 也支持旋转（备选操作）
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
        editor_camera_yaw_ += io.MouseDelta.x * 0.35f;
        editor_camera_pitch_ =
            std::clamp(editor_camera_pitch_ - io.MouseDelta.y * 0.35f, -89.0f,
                       89.0f);
    }

    const float yaw_rad = math::to_radians(editor_camera_yaw_);
    const float pitch_rad = math::to_radians(editor_camera_pitch_);
    const math::Vector3f forward(
        std::cos(yaw_rad) * std::cos(pitch_rad),
        std::sin(pitch_rad),
        std::sin(yaw_rad) * std::cos(pitch_rad));
    const math::Vector3f right =
        forward.cross(math::Vector3f::up()).normalized();
    const math::Vector3f up = right.cross(forward).normalized();

    // 中键拖拽 → 平移相机
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
        const float scale = editor_camera_distance_ * 0.0025f * dt * 60.0f;
        const float dx = io.MouseDelta.x;
        const float dy = io.MouseDelta.y;
        editor_camera_target_ += right * (-dx * scale);
        editor_camera_target_ += up * (dy * scale);
    }

    // 滚轮 → 缩放
    if (std::fabs(io.MouseWheel) > 0.0001f) {
        editor_camera_distance_ =
            std::clamp(editor_camera_distance_ *
                           std::exp(-io.MouseWheel * 0.12f),
                       0.5f, 200.0f);
    }

    editor_camera_.set_position(editor_camera_target_ -
                                forward * editor_camera_distance_);
    editor_camera_.set_yaw(editor_camera_yaw_);
    editor_camera_.set_pitch(editor_camera_pitch_);
}

void EditorApp::collect_scene_lights() {
    editor_lights_.clear();
    if (!scene_) return;

    scene_->foreach([this](scene::Entity* entity) {
        auto* light = entity->get_component<components::Light>();
        if (!light || !light->enabled) return;

        render::RenderPipeline::Light out;
        out.type =
            static_cast<render::RenderPipeline::LightType>(light->light_type);
        out.position = entity->world_transform().translation();
        out.direction = light->direction;
        out.color = light->color;
        out.intensity = light->intensity;
        out.range = light->range;
        out.spot_angle = light->spot_angle;
        out.spot_softness = light->spot_softness;
        editor_lights_.push_back(out);
    });

    if (editor_lights_.empty()) {
        render::RenderPipeline::Light fallback;
        fallback.type = render::RenderPipeline::LightType::Directional;
        fallback.direction = math::Vector3f(-0.4f, -1.0f, -0.3f).normalized();
        fallback.intensity = 1.0f;
        editor_lights_.push_back(fallback);
    }
}

void EditorApp::apply_deferred_deletes() {
    auto& queue = hierarchy_.queued_deletes();
    if (queue.empty() || !scene_) return;

    for (scene::Entity* entity : queue) {
        if (!entity) continue;
        if (scene_->find_entity_by_uuid(entity->uuid()) != nullptr) {
            command_stack_.push(
                std::make_unique<DeleteEntityCommand>(scene_, entity));
        }
    }
    hierarchy_.clear_queued_deletes();
}

void EditorApp::resize_viewport_if_needed(int width, int height) {
    if (!viewport_resize_pending_) return;
    viewport_resize_pending_ = false;

    width = std::max(width, 32);
    height = std::max(height, 32);
    if (width == viewport_texture_width_ && height == viewport_texture_height_) {
        return;
    }

    const bool was_running = render_ctx_ && render_ctx_->is_running();
    if (was_running) {
        render_ctx_->pause_render_thread();
    }

    const bool resized = pipeline_->resize_render_targets(width, height);
    if (resized) {
        viewport_texture_width_ = width;
        viewport_texture_height_ = height;
    }

    if (resized && vulkan_) {
        // Vulkan 的 pipeline 在创建时绑定 render pass；resize 重建了 FBO /
        // render pass，必须热重载所有 shader，让新管线绑定到新 render pass。
        pipeline_->hot_reload();
    }

    if (was_running) {
        render_ctx_->resume_render_thread();
    }
}

uint64_t EditorApp::viewport_texture_id() const {
    if (!pipeline_ || !imgui_ || !imgui_->backend()) return 0;
    render::ITexture* texture = pipeline_->viewport_color_texture();
    if (!texture) return 0;
    return imgui_->backend()->imgui_texture_id(texture);
}

} // namespace gryce_engine::editor
