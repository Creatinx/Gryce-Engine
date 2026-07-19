#include "editor_app.h"

#include <iostream>
#include <format>
#include <fstream>
#include <vector>
#include <string>
#include <memory>
#include <filesystem>
#include <thread>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <utility>
#include <future>
#include <algorithm>

#include <GL/glew.h>
#include <GLFW/glfw3.h>

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <windows.h>
#endif

#include <imgui.h>
#include <ImGuizmo.h>

#include "platform/window.h"
#include "platform/input.h"
#include "render/render_context.h"
#include "render/rhi_handle.h"
#include "render/mesh.h"
#include "render/shader.h"
#include "render/render2d.h"
#include "render/opengl/imgui_renderer.h"
#include "components/2d/basic_rect.h"
#include "components/2d/shape.h"
#include "components/2d/label.h"
#include "components/mesh_renderer.h"
#include "components/physics_body.h"
#include "components/node2d.h"
#include "components/node3d.h"
#include "components/camera.h"
#include "components/light.h"
#include "components/static_body.h"
#include "components/rigid_body.h"
#include "components/box_collider.h"
#include "components/sphere_collider.h"
#include "components/audio_source.h"
#include "components/component_factory.h"
#include "assets/asset_manager.h"
#include "assets/mesh_data.h"
#include "resources/project.h"
#include "resources/resource_path.h"
#include "scene/scene.h"
#include "scene/scene_serializer.h"
#include "scene/entity.h"
#include "math/math.h"
#include "math/ray.h"
#include "math/camera.h"
#include "utils/glog/glog_lib.h"
#include "utils/frame_limiter.h"
#include "ecs/world.h"
#include "ecs/systems/physics_system_3d.h"
#include "ecs/systems/render_system_2d.h"
#include "ecs/systems/render_system_3d.h"
#include "render/render_pipeline.h"

#include "editor_camera.h"
#include "panel_manager.h"
#include "panels/hierarchy_panel.h"
#include "panels/inspector_panel.h"
#include "panels/console_panel.h"
#include "panels/viewport_panel.h"
#include "panels/project_panel.h"
#include "ui/editor_theme.h"
#include "ui/settings_window.h"
#include "localization/localization.h"

using namespace gryce_engine;

// ---------------------------------------------------------------------------
// 查找编辑器项目根目录：
// 1) 先定位引擎仓库根（含 CMakeLists.txt 与 core/）；
// 2) 优先使用 editor/project/（编辑器自带项目，含 project.gryce）；
// 3) 缺失时回退 examples/3dtest/（开发期共享资源）；
// 4) 都找不到则退化为当前工作目录。
// ---------------------------------------------------------------------------
static std::filesystem::path find_project_root() {
    std::filesystem::path exe_path;
#ifdef _WIN32
    wchar_t buffer[MAX_PATH];
    if (GetModuleFileNameW(nullptr, buffer, MAX_PATH) > 0) {
        exe_path = std::filesystem::path(buffer);
    }
#else
    exe_path = std::filesystem::canonical("/proc/self/exe");
#endif
    std::filesystem::path dir = exe_path.parent_path();
    std::filesystem::path engine_root;
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        if (std::filesystem::exists(dir / "CMakeLists.txt") &&
            std::filesystem::is_directory(dir / "core")) {
            engine_root = dir;
            break;
        }
        dir = dir.parent_path();
    }
    if (!engine_root.empty()) {
        const std::filesystem::path editor_project = engine_root / "editor" / "project";
        if (std::filesystem::exists(editor_project / "project.gryce")) {
            return editor_project;
        }
        const std::filesystem::path dev_project = engine_root / "examples" / "3dtest";
        if (std::filesystem::exists(dev_project / "project.gryce")) {
            return dev_project;
        }
    }
    return std::filesystem::current_path();
}

// ---------------------------------------------------------------------------
// 场景辅助函数（与原 demo 相同：2D FPS label + 图形、物理演示 cube/ground）
// ---------------------------------------------------------------------------
static std::unique_ptr<scene::Scene> create_demo_scene(float screen_w, float screen_h) {
    auto scene = std::make_unique<scene::Scene>("main");

    // 左上角 FPS 背景
    {
        scene::Entity* fps_bg = scene->create_entity("FPS_BG");
        fps_bg->transform()->position = math::Vector3f(5.0f, 5.0f, 0.0f);
        fps_bg->add_component<components::d2::basic_rect::ColorRect>(120.0f, 30.0f, render::Color(0.2f, 0.2f, 0.2f, 0.8f));
    }

    // 左上角 FPS 文字
    {
        scene::Entity* fps_label = scene->create_entity("FPS_Label");
        fps_label->transform()->position = math::Vector3f(15.0f, 23.0f, 0.0f);
        fps_label->add_component<components::d2::text::Label>("FPS: 0.0", 18.0f, render::Color::white());
    }

    // 右下角图形测试面板
    float panel_w = 200.0f, panel_h = 200.0f;
    float panel_x = screen_w - panel_w - 20.0f;
    float panel_y = screen_h - panel_h - 20.0f;
    float panel_cx = panel_x + panel_w * 0.5f;
    float panel_cy = panel_y + panel_h * 0.5f;

    {
        scene::Entity* panel_bg = scene->create_entity("Panel_BG");
        panel_bg->transform()->position = math::Vector3f(panel_x, panel_y, 0.0f);
        panel_bg->add_component<components::d2::basic_rect::ColorRect>(panel_w, panel_h, render::Color::gray(0.15f));
    }

    {
        scene::Entity* circle = scene->create_entity("Circle");
        circle->transform()->position = math::Vector3f(panel_cx, panel_cy, 0.0f);
        circle->add_component<components::d2::shape::Circle>(50.0f, 32, render::Color::orange());
    }

    {
        scene::Entity* triangle = scene->create_entity("Triangle");
        triangle->transform()->position = math::Vector3f(panel_cx, panel_cy, 0.0f);
        std::vector<math::Vector2f> triangle_pts = {
            {-70.0f,  70.0f},
            {  0.0f, -70.0f},
            { 70.0f,  70.0f}
        };
        triangle->add_component<components::d2::shape::Polygon>(triangle_pts, render::Color::cyan());
    }

    {
        scene::Entity* pentagon = scene->create_entity("Pentagon");
        pentagon->transform()->position = math::Vector3f(panel_cx, panel_cy, 0.0f);
        std::vector<math::Vector2f> pentagon_pts;
        for (int i = 0; i < 5; ++i) {
            float angle = 2.0f * math::to_radians(72.0f) * static_cast<float>(i) - math::to_radians(90.0f);
            pentagon_pts.push_back({30.0f * std::cos(angle), 30.0f * std::sin(angle)});
        }
        pentagon->add_component<components::d2::shape::Polygon>(pentagon_pts, render::Color::magenta());
    }

    return scene;
}

static void upload_scene_meshes(scene::Scene& scene, render::RenderContext& ctx) {
    scene.foreach([&](scene::Entity* entity) {
        auto* mr = entity->get_component<components::MeshRenderer>();
        if (!mr || mr->mesh_path.empty()) return;

        const assets::MeshData* data = assets::AssetManager::instance().load_mesh(mr->mesh_path);
        if (!data) return;

        render::IMesh* gpu_mesh = mr->upload_to_gpu(&ctx, data);
        if (gpu_mesh) {
            GLOG_INFO("Pre-uploaded mesh '{}' for entity '{}'", mr->mesh_path, entity->name());
        }
    });
}

static std::filesystem::file_time_type get_scene_write_time(const std::string& scene_path) {
    std::string resolved = resources::ResourcePath::resolve(scene_path);
    std::error_code ec;
    auto time = std::filesystem::last_write_time(resolved, ec);
    if (ec) {
        return std::filesystem::file_time_type::min();
    }
    return time;
}

static std::unique_ptr<scene::Scene> try_reload_scene(
    const std::string& scene_path,
    std::unique_ptr<scene::Scene> current,
    std::filesystem::file_time_type new_time,
    std::filesystem::file_time_type& last_write_time) {

    GLOG_INFO("Scene file changed, reloading '{}'", scene_path);
    auto reloaded = scene::SceneSerializer::load_from_file(scene_path);
    if (!reloaded) {
        GLOG_ERROR("Failed to reload scene from '{}'", scene_path);
        last_write_time = new_time; // 避免解析失败时反复重试
        return current;
    }

    last_write_time = new_time;
    GLOG_INFO("Scene reloaded successfully");
    return reloaded;
}

static scene::Entity* create_cube_entity(scene::Scene& scene, const std::string& name,
                                          bool dynamic = false) {
    scene::Entity* e = scene.create_entity(name);
    e->transform()->position = dynamic ? math::Vector3f(0.0f, 3.0f, 0.0f)
                                       : math::Vector3f(0.0f, 0.0f, 0.0f);
    auto* mr = e->add_component<components::MeshRenderer>("res:/models/cube_pbr.obj");
    if (mr && mr->material) {
        mr->material->name = dynamic ? "DynamicCube" : "CubePBR";
        mr->material->albedo_map_path = "res:/textures/cube_albedo.png";
        mr->material->normal_map_path = "res:/textures/cube_normal.png";
        mr->material->roughness_map_path = "res:/textures/cube_roughness.png";
        mr->material->metallic_map_path = "res:/textures/cube_metallic.png";
        mr->material->ao_map_path = "res:/textures/cube_ao.png";
    }

    if (dynamic) {
        e->add_component<components::RigidBody>();
        e->add_component<components::BoxCollider>();
    }
    return e;
}

static scene::Entity* create_ground_entity(scene::Scene& scene, const std::string& name) {
    scene::Entity* e = scene.create_entity(name);
    e->transform()->position = math::Vector3f(0.0f, -2.0f, 0.0f);
    e->transform()->scale = math::Vector3f(10.0f, 0.5f, 10.0f);

    auto* mr = e->add_component<components::MeshRenderer>("res:/models/cube_pbr.obj");
    if (mr && mr->material) {
        mr->material->name = "Ground";
        mr->material->use_albedo_map = false;
        mr->material->use_normal_map = false;
        mr->material->use_roughness_map = false;
        mr->material->use_metallic_map = false;
        mr->material->use_ao_map = false;
        mr->material->albedo_color = math::Vector3f(0.2f, 0.5f, 0.2f);
        mr->material->roughness = 0.9f;
        mr->material->metallic = 0.0f;
        mr->material->ao = 1.0f;
    }

    e->add_component<components::StaticBody>();
    auto* col = e->add_component<components::BoxCollider>();
    col->size = math::Vector3f(1.0f, 1.0f, 1.0f); // 缩放由 Transform.scale 处理
    return e;
}

static void ensure_physics_demo_entities(scene::Scene& scene) {
    if (!scene.find_entity_by_name("Ground")) {
        create_ground_entity(scene, "Ground");
    }

    scene::Entity* cube = scene.find_entity_by_name("Cube");
    if (!cube) {
        cube = create_cube_entity(scene, "Cube", true);
    } else {
        // 确保已有的 Cube 是动态的，并放到空中准备下落
        if (!cube->get_component<components::RigidBody>()) {
            cube->add_component<components::RigidBody>();
        }
        if (!cube->get_component<components::BoxCollider>()) {
            cube->add_component<components::BoxCollider>();
        }
        cube->transform()->position = math::Vector3f(0.0f, 3.0f, 0.0f);
        cube->transform()->rotation = math::Quaternionf::identity();
        cube->transform()->scale = math::Vector3f::one();
        if (auto* rb = cube->get_component<components::RigidBody>()) {
            rb->velocity = math::Vector3f::zero();
            rb->acceleration = math::Vector3f::zero();
        }
    }
}

// ---------------------------------------------------------------------------
// 场景默认对象：主摄像机 + 主光源
// ---------------------------------------------------------------------------
static scene::Entity* find_main_camera_entity(scene::Scene& scene) {
    scene::Entity* result = nullptr;
    scene.foreach([&](scene::Entity* entity) {
        auto* cam = entity->get_component<components::Camera>();
        if (!cam || !cam->enabled || !cam->is_main) return;
        // 简单规则：is_main=true 中第一个找到的即可；后续可扩展 priority。
        if (!result) {
            result = entity;
        }
    });
    return result;
}

static void ensure_scene_defaults(scene::Scene& scene, math::Camera& camera) {
    scene::Entity* cam_entity = find_main_camera_entity(scene);
    if (!cam_entity) {
        cam_entity = scene.create_entity("MainCamera");
        cam_entity->transform()->position = camera.position();
        cam_entity->transform()->rotation = math::Quaternionf::from_euler(
            math::to_radians(camera.pitch()), math::to_radians(camera.yaw()), 0.0f);
        auto* cam = cam_entity->add_component<components::Camera>();
        cam->fov = camera.fov();
        cam->near_plane = 0.1f;
        cam->far_plane = 100.0f;
        cam->is_main = true;
        GLOG_INFO("Created default MainCamera entity");
    }

    bool has_light = false;
    scene.foreach([&](scene::Entity* entity) {
        if (entity->get_component<components::Light>()) has_light = true;
    });
    if (!has_light) {
        scene::Entity* light_entity = scene.create_entity("MainLight");
        light_entity->transform()->rotation = math::Quaternionf::from_euler(
            math::to_radians(-30.0f), math::to_radians(-45.0f), 0.0f);
        auto* light = light_entity->add_component<components::Light>();
        light->light_type = components::Light::Type::Directional;
        light->direction = math::Vector3f(-0.3f, -0.7f, -0.6f).normalized();
        light->color = math::Vector3f::one();
        light->intensity = 3.0f;
        GLOG_INFO("Created default MainLight entity");
    }
}

static void sync_active_camera_to_scene(scene::Scene& scene, math::Camera& camera) {
    scene::Entity* cam_entity = find_main_camera_entity(scene);
    if (!cam_entity) return;
    auto* cam = cam_entity->get_component<components::Camera>();
    if (!cam) return;

    // 将编辑器相机的位置/朝向写回 MainCamera 组件，便于保存场景。
    cam_entity->transform()->position = camera.position();
    cam_entity->transform()->rotation = math::Quaternionf::from_euler(
        math::to_radians(camera.pitch()), math::to_radians(camera.yaw()), 0.0f);
}

static void apply_camera_component_to_global(scene::Scene& scene, math::Camera& camera) {
    scene::Entity* cam_entity = find_main_camera_entity(scene);
    if (!cam_entity) return;
    auto* cam = cam_entity->get_component<components::Camera>();
    if (!cam) return;
    camera.set_fov(cam->fov);
    camera.set_near_far(cam->near_plane, cam->far_plane);
}

static std::vector<render::RenderPipeline::Light> collect_lights(scene::Scene& scene) {
    std::vector<render::RenderPipeline::Light> lights;
    scene.foreach([&](scene::Entity* entity) {
        auto* light = entity->get_component<components::Light>();
        if (!light || !light->enabled) return;
        render::RenderPipeline::Light out;
        out.color = light->color;
        out.intensity = light->intensity;
        // 方向光：使用组件显式方向；其他类型暂按方向光处理。
        out.direction = light->direction.normalized();
        lights.push_back(out);
    });
    if (lights.empty()) {
        // 兜底：保证至少有一个方向光
        render::RenderPipeline::Light fallback;
        fallback.direction = math::Vector3f(0.0f, -1.0f, 0.0f);
        fallback.color = math::Vector3f::one();
        fallback.intensity = 1.0f;
        lights.push_back(fallback);
    }
    return lights;
}

// ---------------------------------------------------------------------------
// 点选拾取（M1-E2）：遍历场景中带 MeshRenderer 的实体，
// 用 mesh 顶点的本地 AABB 经世界矩阵变换成世界 AABB 后做射线求交，取最近命中。
// 不依赖碰撞体（无 Collider 的实体也能被选中）；编辑器规模下线扫开销可忽略。
// ---------------------------------------------------------------------------
static bool compute_world_aabb(const assets::MeshData& mesh, const math::Matrix4f& world,
                               math::Vector3f& out_min, math::Vector3f& out_max) {
    if (mesh.vertices.empty()) return false;

    math::Vector3f lo = mesh.vertices[0].position;
    math::Vector3f hi = lo;
    for (const auto& v : mesh.vertices) {
        lo.x = std::min(lo.x, v.position.x);
        lo.y = std::min(lo.y, v.position.y);
        lo.z = std::min(lo.z, v.position.z);
        hi.x = std::max(hi.x, v.position.x);
        hi.y = std::max(hi.y, v.position.y);
        hi.z = std::max(hi.z, v.position.z);
    }

    // 本地 AABB 8 角点经世界矩阵变换后重取包围盒
    out_min = math::Vector3f(1e30f, 1e30f, 1e30f);
    out_max = math::Vector3f(-1e30f, -1e30f, -1e30f);
    for (int i = 0; i < 8; ++i) {
        const math::Vector3f corner((i & 1) ? hi.x : lo.x,
                                    (i & 2) ? hi.y : lo.y,
                                    (i & 4) ? hi.z : lo.z);
        const math::Vector3f p = world.transform_point(corner);
        out_min.x = std::min(out_min.x, p.x);
        out_min.y = std::min(out_min.y, p.y);
        out_min.z = std::min(out_min.z, p.z);
        out_max.x = std::max(out_max.x, p.x);
        out_max.y = std::max(out_max.y, p.y);
        out_max.z = std::max(out_max.z, p.z);
    }
    return true;
}

static scene::Entity* pick_entity(scene::Scene& scene, const math::Ray& ray) {
    scene::Entity* best_entity = nullptr;
    float best_t = 1e30f;

    scene.foreach([&](scene::Entity* entity) {
        auto* mr = entity->get_component<components::MeshRenderer>();
        if (!mr || !mr->enabled || mr->mesh_path.empty()) return;

        const assets::MeshData* mesh = assets::AssetManager::instance().load_mesh(mr->mesh_path);
        if (!mesh) return;

        math::Vector3f bmin, bmax;
        if (!compute_world_aabb(*mesh, entity->world_transform(), bmin, bmax)) return;

        float t = 0.0f;
        if (math::ray_intersect_aabb(ray, bmin, bmax, t) && t < best_t) {
            best_t = t;
            best_entity = entity;
        }
    });
    return best_entity;
}

namespace gryce_engine::editor {

// ---------------------------------------------------------------------------
// EditorApp::run — 编辑器主循环
// ---------------------------------------------------------------------------
int EditorApp::run(int argc, char* argv[]) {
    std::cout << "Gryce Engine Editor v0.1.0" << std::endl;

    // 解析命令行参数
    render::RenderAPI selected_api = render::RenderAPI::OpenGL;
    bool screenshot_mode = false;
    bool vulkan_validation = false; // 默认关闭 validation，需要时通过 --vulkan-validation 开启
    bool test_play_mode = false;    // --test-play-mode：自动进入/退出 Play Mode，用于 CI
    float auto_close_seconds = 0.0f; // --auto-close N：运行 N 秒后自动关闭，用于 CI/关机测试
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--vulkan") == 0) {
            selected_api = render::RenderAPI::Vulkan;
        } else if (std::strcmp(argv[i], "--screenshot") == 0) {
            screenshot_mode = true;
        } else if (std::strcmp(argv[i], "--vulkan-validation") == 0) {
            vulkan_validation = true;
        } else if (std::strcmp(argv[i], "--test-play-mode") == 0) {
            test_play_mode = true;
        } else if (std::strcmp(argv[i], "--auto-close") == 0 && i + 1 < argc) {
            auto_close_seconds = static_cast<float>(std::atof(argv[++i]));
        }
    }

    utils::glog_initialize();
    utils::GLog::instance().set_min_level(utils::LogLevel::Info);
    // 安装内存 sink：控制台输出不变，同时供 Console 面板读取
    utils::GLog::instance().set_logger(
        std::make_unique<utils::MemoryLogSink>(std::make_unique<utils::ConsoleLogger>()));

    // 设置项目根目录（自动从可执行文件位置向上查找）
    const std::filesystem::path project_root = find_project_root();
    resources::Project::instance().set_root(project_root.string());
    components::register_builtin_components();

    // 初始化 GLFW
    if (!platform::Window::init_sdk()) {
        GLOG_ERROR("Failed to initialize GLFW");
        return -1;
    }

    // 创建窗口：OpenGL 需要 context，Vulkan 用 NoApi
    platform::WindowContextType window_ctx = (selected_api == render::RenderAPI::Vulkan)
                                                 ? platform::WindowContextType::NoApi
                                                 : platform::WindowContextType::OpenGL;
    platform::Window window("Gryce Engine Editor", 1600, 900,
                            platform::WindowMode::Windowed, window_ctx);
    if (!window.is_valid()) {
        GLOG_ERROR("Failed to create window");
        platform::Window::shutdown_sdk();
        return -1;
    }
    if (selected_api == render::RenderAPI::OpenGL) {
        window.set_vsync(false);
    }

    const bool is_vulkan = (selected_api == render::RenderAPI::Vulkan);

    // 创建渲染上下文
    render::RenderContext render_ctx;
    render_ctx.set_validation_enabled(vulkan_validation);
    if (!render_ctx.init(window.native_handle(), selected_api)) {
        GLOG_ERROR("Failed to initialize render context");
        platform::Window::shutdown_sdk();
        return -1;
    }

    // 窗口大小变化时立即更新 viewport
    window.set_resize_callback([&](int w, int h) {
        render_ctx.set_viewport(0, 0, w, h);
    });

    // 创建 2D 渲染器（OpenGL / Vulkan 各自后端）
    auto renderer2d = render_ctx.create_renderer2d();
    if (renderer2d) {
        renderer2d->init(&render_ctx);
    }

    // 初始化 ImGui（必须在 render_ctx.start() 之前，GL context 还在主线程）
    render::ImGuiRenderer imgui;
    auto imgui_backend = render_ctx.create_imgui_backend();
    imgui.init(window.native_handle(), std::move(imgui_backend));

    // 布局持久化：ini 写到项目根目录而不是工作目录/build 目录
    static const std::string imgui_ini_path = (project_root / "editor_imgui.ini").string();
    ImGui::GetIO().IniFilename = imgui_ini_path.c_str();

    // Fluent Design 主题与字体（加载持久化配置，失败则使用默认深色主题）
    editor::EditorSettings editor_settings = editor::SettingsWindow::load(project_root.string());
    editor::ThemeConfig& theme_config = editor_settings.theme;
    editor::ThemePreset& theme_preset = editor_settings.theme_preset;
    editor::apply_theme(theme_preset, theme_config);

    // 多语言本地化（加载语言文件，失败则回退英语）
    editor::Localization::instance().load(editor_settings.appliance.language, project_root.string());
    editor::Localization::instance().set_light_theme(theme_preset == editor::ThemePreset::Light);

    // 设置窗口
    editor::SettingsWindow settings_window;

    // 编辑器面板框架
    PanelManager panel_manager;
    auto* hierarchy_panel = panel_manager.add_panel<HierarchyPanel>();
    auto* inspector_panel = panel_manager.add_panel<InspectorPanel>();
    auto* viewport_panel = panel_manager.add_panel<ViewportPanel>();
    panel_manager.add_panel<ConsolePanel>();
    auto* project_panel = panel_manager.add_panel<ProjectPanel>();
    viewport_panel->set_imgui_backend(imgui.backend());

    // Play Mode（M1-E4）：运行时预览，退出时从快照恢复场景
    bool play_mode_active = false;
    std::string play_mode_snapshot;
    viewport_panel->set_editing_enabled(!play_mode_active);

    // 输入（仅保留 F1 线框调试开关；相机控制走 EditorCamera / ImGui IO）
    platform::InputManager input;
    input.update(&window);

    // 编辑器自由飞行相机（不依赖场景相机实体）
    EditorCamera editor_camera;
    math::Camera& camera = editor_camera.camera();

    // 视口面板接线：相机 + 选中实体（Hierarchy UUID 弱引用解析）
    viewport_panel->set_camera(&camera);
    viewport_panel->set_selection_provider(
        [hierarchy_panel]() { return hierarchy_panel->selected_entity(); });

    // 加载或创建场景（必须在 render_ctx.start() 之前，因为上传网格需要主线程 GL context）
    std::string scene_path = "res:/scenes/main.gesc";
    std::unique_ptr<scene::Scene> current_scene = scene::SceneSerializer::load_from_file(scene_path);
    if (!current_scene) {
        GLOG_INFO("No existing scene found at '{}', creating demo scene", scene_path);
        current_scene = create_demo_scene(1600.0f, 900.0f);
        scene::SceneSerializer::save_to_file(*current_scene, scene_path);
    } else {
        GLOG_INFO("Scene loaded from '{}'", scene_path);
    }

    // 缓存 FPS Label 组件指针，便于每帧更新文字
    components::d2::text::Label* fps_label = nullptr;
    if (current_scene) {
        scene::Entity* fps_entity = current_scene->find_entity_by_name("FPS_Label");
        if (fps_entity) {
            fps_label = fps_entity->get_component<components::d2::text::Label>();
        }

        // 确保物理演示场景有地面和可下落的 cube
        ensure_physics_demo_entities(*current_scene);

        // 预上传所有 3D 网格到 GPU（必须在 start() 之前，主线程持有 GL context）
        upload_scene_meshes(*current_scene, render_ctx);

        // 确保场景有主摄像机和主光源
        ensure_scene_defaults(*current_scene, camera);
    }

    // 场景文件热重载：记录最后修改时间
    std::filesystem::file_time_type scene_last_write = get_scene_write_time(scene_path);
    double scene_reload_timer = 0.0;

    // 创建渲染管线（必须在 start() 之前，主线程持有 GL context）。
    // OpenGL 端开启视口离屏输出：tonemap 结果写入独立 FBO 供 Viewport 面板采样；
    // Vulkan 端 descriptor 注册未实现，本轮仍直接渲染到屏幕、Viewport 显示占位。
    render::RenderPipeline pipeline;
    pipeline.set_viewport_output_enabled(!is_vulkan);
    if (!pipeline.init(&render_ctx, "res:/shaders")) {
        GLOG_ERROR("Failed to initialize render pipeline");
        render_ctx.shutdown();
        platform::Window::shutdown_sdk();
        return -1;
    }
    viewport_panel->set_pipeline(&pipeline);

    // 创建 ECS World 并注册系统
    ecs::World world;
    if (current_scene) {
        world.attach_scene(std::move(current_scene));
        world.add_system<ecs::PhysicsSystem3D>();
        world.add_system<ecs::RenderSystem3D>(&pipeline);
        if (renderer2d) {
            world.add_system<ecs::RenderSystem2D>(renderer2d.get());
        }
        world.init();
    }

    // -------------------------------------------------------------------
    // File 菜单：场景保存 / 加载（M1-E2）
    // -------------------------------------------------------------------
    bool save_as_popup_requested = false;
    bool open_popup_requested = false;
    char save_as_buf[256] = "res:/scenes/main.gesc";
    char open_buf[256] = "res:/scenes/main.gesc";

    // FPS Label 组件指针在场景替换后需要重新查找
    auto refresh_fps_label = [&]() {
        fps_label = nullptr;
        if (world.scene()) {
            scene::Entity* fps_entity = world.scene()->find_entity_by_name("FPS_Label");
            if (fps_entity) {
                fps_label = fps_entity->get_component<components::d2::text::Label>();
            }
        }
    };

    auto save_scene = [&](const std::string& path) {
        if (!world.scene()) return;
        if (scene::SceneSerializer::save_to_file(*world.scene(), path)) {
            scene_path = path;
            // 保存会改写文件 mtime，刷新缓存避免紧接着触发一次热重载
            scene_last_write = get_scene_write_time(scene_path);
            world.scene()->mark_saved();
            GLOG_INFO("Scene saved to '{}'", path);
        } else {
            GLOG_ERROR("Failed to save scene to '{}'", path);
        }
    };

    auto restore_scene_from_snapshot = [&](const std::string& snapshot_json) {
        nlohmann::json json;
        try {
            json = nlohmann::json::parse(snapshot_json);
        } catch (const std::exception& e) {
            GLOG_ERROR("Play Mode: failed to parse scene snapshot: {}", e.what());
            return;
        }
        auto loaded = scene::SceneSerializer::deserialize(json);
        if (!loaded) {
            GLOG_ERROR("Play Mode: failed to deserialize scene snapshot");
            return;
        }
        world.detach_scene();
        hierarchy_panel->clear_selection();
        render_ctx.pause_render_thread();
        ensure_physics_demo_entities(*loaded);
        upload_scene_meshes(*loaded, render_ctx);
        ensure_scene_defaults(*loaded, camera);
        world.attach_scene(std::move(loaded));
        render_ctx.resume_render_thread();

        refresh_fps_label();
        GLOG_INFO("Play Mode: scene restored from snapshot");
    };

    auto exit_play_mode = [&]() {
        if (!play_mode_active) return;
        play_mode_active = false;
        viewport_panel->set_editing_enabled(true);
        hierarchy_panel->set_drag_enabled(true);
        inspector_panel->set_read_only(false);
        if (!play_mode_snapshot.empty()) {
            restore_scene_from_snapshot(play_mode_snapshot);
            play_mode_snapshot.clear();
        }
        GLOG_INFO("Play Mode: exited");
    };

    auto enter_play_mode = [&]() {
        if (!world.scene() || play_mode_active) return;
        play_mode_snapshot = scene::SceneSerializer::serialize(*world.scene()).dump();
        play_mode_active = true;
        viewport_panel->set_editing_enabled(false);
        hierarchy_panel->set_drag_enabled(false);
        inspector_panel->set_read_only(true);
        GLOG_INFO("Play Mode: entered");
    };

    auto toggle_play_mode = [&]() {
        if (play_mode_active) {
            exit_play_mode();
        } else {
            enter_play_mode();
        }
    };

    auto open_scene = [&](const std::string& path) {
        // 打开新场景前若处于 Play Mode，先退出并丢弃快照
        if (play_mode_active) {
            exit_play_mode();
        }

        auto loaded = scene::SceneSerializer::load_from_file(path);
        if (!loaded) {
            GLOG_ERROR("Failed to open scene from '{}'", path);
            return;
        }
        // 与热重载相同的替换流程：暂停渲染线程后上传 GPU 资源
        world.detach_scene();
        hierarchy_panel->clear_selection();
        render_ctx.pause_render_thread();
        ensure_physics_demo_entities(*loaded);
        upload_scene_meshes(*loaded, render_ctx);
        ensure_scene_defaults(*loaded, camera);
        world.attach_scene(std::move(loaded));
        render_ctx.resume_render_thread();

        scene_path = path;
        scene_last_write = get_scene_write_time(scene_path);
        refresh_fps_label();
        GLOG_INFO("Scene opened from '{}'", path);
    };

    // -----------------------------------------------------------------------
    // 资源浏览器（Project 面板）回调：双击 / 拖拽
    // -----------------------------------------------------------------------
    auto to_lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    auto extension_of = [&](const std::string& path) {
        return to_lower(std::filesystem::path(path).extension().string());
    };
    auto is_scene_file = [&](const std::string& path) {
        return extension_of(path) == ".gesc";
    };
    auto is_mesh_file = [&](const std::string& path) {
        const std::string ext = extension_of(path);
        return ext == ".obj" || ext == ".fbx" || ext == ".gltf" || ext == ".glb";
    };
    auto is_texture_file = [&](const std::string& path) {
        const std::string ext = extension_of(path);
        return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" ||
               ext == ".tga" || ext == ".dds" || ext == ".ktx" || ext == ".hdr";
    };
    auto file_stem = [&](const std::string& path) {
        return std::filesystem::path(path).stem().string();
    };

    auto apply_texture_to_material = [&](components::MeshRenderer* mr, const std::string& tex_path) {
        if (!mr || !mr->material) return;
        render::Material* mat = mr->material.get();

        const std::string lower = to_lower(file_stem(tex_path));
        if (lower.find("normal") != std::string::npos) {
            mat->normal_map_path = tex_path;
            mat->use_normal_map = true;
        } else if (lower.find("roughness") != std::string::npos) {
            mat->roughness_map_path = tex_path;
            mat->use_roughness_map = true;
        } else if (lower.find("metallic") != std::string::npos || lower.find("metal") != std::string::npos) {
            mat->metallic_map_path = tex_path;
            mat->use_metallic_map = true;
        } else if (lower.find("ao") != std::string::npos || lower.find("ambient") != std::string::npos) {
            mat->ao_map_path = tex_path;
            mat->use_ao_map = true;
        } else {
            mat->albedo_map_path = tex_path;
            mat->use_albedo_map = true;
        }

        // 若网格已上传，材质也已在 GPU，需要异步销毁旧纹理并重新上传新材质
        if (mr->gpu_mesh()) {
            auto token = mr->alive_token();
            render_ctx.push_command([mat, ctx = &render_ctx, token](render::IRenderBackend*) {
                if (!token || !token->load(std::memory_order_acquire)) return;
                mat->destroy_gpu(ctx);
                mat->upload_to_gpu(ctx);
            });
        }
    };

    auto instantiate_mesh_entity = [&](scene::Entity* parent, const std::string& mesh_path) -> scene::Entity* {
        scene::Scene* scene = world.scene();
        if (!scene) return nullptr;

        const std::string name = file_stem(mesh_path);
        scene::Entity* entity = nullptr;
        if (parent) {
            auto child = std::make_unique<scene::Entity>(name);
            entity = parent->add_child(std::move(child));
        } else {
            entity = scene->create_entity(name);
        }
        if (!entity) return nullptr;

        auto* mr = entity->add_component<components::MeshRenderer>(mesh_path);
        if (mr && mr->material) {
            mr->material->name = name;
        }
        hierarchy_panel->select(entity->uuid());
        GLOG_INFO("Project: instantiated mesh '{}' as entity '{}'", mesh_path, name);
        return entity;
    };

    auto create_textured_cube = [&](scene::Entity* parent, const std::string& tex_path) -> scene::Entity* {
        scene::Entity* entity = instantiate_mesh_entity(parent, "res:/models/cube_pbr.obj");
        if (!entity) return nullptr;
        auto* mr = entity->get_component<components::MeshRenderer>();
        if (mr && mr->material) {
            mr->material->name = file_stem(tex_path);
            apply_texture_to_material(mr, tex_path);
        }
        return entity;
    };

    auto apply_file_to_entity = [&](scene::Entity* entity, const std::string& path) {
        if (!entity) return;
        if (is_mesh_file(path)) {
            // 替换为新的 MeshRenderer，渲染线程会自动上传
            if (auto* old = entity->get_component<components::MeshRenderer>()) {
                entity->remove_component(old);
            }
            auto* mr = entity->add_component<components::MeshRenderer>(path);
            if (mr && mr->material) {
                mr->material->name = file_stem(path);
            }
            GLOG_INFO("Project: set mesh '{}' on entity '{}'", path, entity->name());
        } else if (is_texture_file(path)) {
            auto* mr = entity->get_component<components::MeshRenderer>();
            if (!mr) {
                mr = entity->add_component<components::MeshRenderer>("res:/models/cube_pbr.obj");
                if (mr && mr->material) mr->material->name = "TexturedCube";
            }
            if (mr) {
                apply_texture_to_material(mr, path);
                GLOG_INFO("Project: set texture '{}' on entity '{}'", path, entity->name());
            }
        } else {
            GLOG_WARN("Project: dropping '{}' onto Inspector is not supported", path);
        }
    };

    auto handle_activate_file = [&](const std::string& path) {
        if (play_mode_active) {
            GLOG_WARN("Project: cannot activate resources while Play Mode is active");
            return;
        }
        if (is_scene_file(path)) {
            open_scene(path);
        } else if (is_mesh_file(path)) {
            instantiate_mesh_entity(nullptr, path);
        } else if (is_texture_file(path)) {
            create_textured_cube(nullptr, path);
        } else {
            GLOG_WARN("Project: double-click on '{}' is not supported", path);
        }
    };

    project_panel->on_activate_file = handle_activate_file;

    viewport_panel->set_drop_handler([&](const std::string& path) {
        if (play_mode_active) {
            GLOG_WARN("Project: cannot drop resources while Play Mode is active");
            return;
        }
        if (is_scene_file(path)) {
            open_scene(path);
        } else if (is_mesh_file(path)) {
            instantiate_mesh_entity(nullptr, path);
        } else if (is_texture_file(path)) {
            create_textured_cube(nullptr, path);
        }
    });

    hierarchy_panel->set_drop_handler([&](scene::Entity* target, const std::string& path) {
        if (play_mode_active) {
            GLOG_WARN("Project: cannot drop resources while Play Mode is active");
            return;
        }
        if (is_scene_file(path)) {
            open_scene(path);
        } else if (is_mesh_file(path)) {
            instantiate_mesh_entity(target, path);
        } else if (is_texture_file(path)) {
            if (target) {
                apply_file_to_entity(target, path);
            } else {
                create_textured_cube(nullptr, path);
            }
        }
    });

    inspector_panel->set_drop_handler([&](scene::Entity* entity, const std::string& path) {
        if (play_mode_active) {
            GLOG_WARN("Project: cannot drop resources while Play Mode is active");
            return;
        }
        apply_file_to_entity(entity, path);
    });

    panel_manager.set_menu_bar_hook([&]() {
        if (ImGui::BeginMenu(editor::tr("menu.file"))) {
            if (ImGui::MenuItem(editor::tr("menu.save_scene"), "Ctrl+S")) {
                save_scene(scene_path);
            }
            if (ImGui::MenuItem(editor::tr("menu.save_scene_as"))) {
                std::strncpy(save_as_buf, scene_path.c_str(), sizeof(save_as_buf) - 1);
                save_as_buf[sizeof(save_as_buf) - 1] = '\0';
                save_as_popup_requested = true;
            }
            if (ImGui::MenuItem(editor::tr("menu.open_scene"))) {
                std::strncpy(open_buf, scene_path.c_str(), sizeof(open_buf) - 1);
                open_buf[sizeof(open_buf) - 1] = '\0';
                open_popup_requested = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem(editor::tr("menu.settings"))) {
                settings_window.open();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(editor::tr("menu.play"))) {
            if (ImGui::MenuItem(play_mode_active ? editor::tr("menu.play_stop") : editor::tr("menu.play_play"), "Ctrl+P")) {
                toggle_play_mode();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(editor::tr("menu.view"))) {
            if (ImGui::BeginMenu(editor::tr("menu.view_theme"))) {
                bool is_dark = (theme_preset == editor::ThemePreset::Dark);
                if (ImGui::MenuItem(editor::tr("menu.view_theme_dark"), nullptr, &is_dark)) {
                    theme_preset = editor::ThemePreset::Dark;
                    editor::apply_theme(theme_preset, theme_config);
                    editor::Localization::instance().set_light_theme(false);
                    editor::SettingsWindow::save(project_root.string(), editor_settings);
                }
                bool is_light = (theme_preset == editor::ThemePreset::Light);
                if (ImGui::MenuItem(editor::tr("menu.view_theme_light"), nullptr, &is_light)) {
                    theme_preset = editor::ThemePreset::Light;
                    editor::apply_theme(theme_preset, theme_config);
                    editor::Localization::instance().set_light_theme(true);
                    editor::SettingsWindow::save(project_root.string(), editor_settings);
                }
                ImGui::EndMenu();
            }
            if (ImGui::DragFloat(editor::tr("menu.view_accent_hue"), &theme_config.accent_hue, 0.01f, 0.0f, 1.0f)) {
                editor::apply_theme(theme_preset, theme_config);
                editor::SettingsWindow::save(project_root.string(), editor_settings);
            }
            if (ImGui::DragFloat(editor::tr("menu.view_font_size"), &theme_config.font_size, 0.5f, 8.0f, 32.0f)) {
                editor::apply_theme(theme_preset, theme_config);
                editor::SettingsWindow::save(project_root.string(), editor_settings);
            }
            if (ImGui::DragFloat(editor::tr("menu.view_rounding"), &theme_config.rounding, 0.5f, 0.0f, 16.0f)) {
                editor::apply_theme(theme_preset, theme_config);
                editor::SettingsWindow::save(project_root.string(), editor_settings);
            }
            if (ImGui::Checkbox(editor::tr("menu.view_shadow"), &theme_config.shadow)) {
                editor::apply_theme(theme_preset, theme_config);
                editor::SettingsWindow::save(project_root.string(), editor_settings);
            }
            ImGui::EndMenu();
        }
    });

    // 启动渲染线程（此后主线程不再持有 GL context）
    render_ctx.start();

    if (screenshot_mode) {
        const std::string screenshot_path = is_vulkan
                                                ? "D:/Gryce-Engine/screenshot_vulkan.bmp"
                                                : "D:/Gryce-Engine/screenshot_opengl.bmp";
        render_ctx.request_screenshot(screenshot_path);
        GLOG_INFO("Screenshot requested on next frame: '{}'", screenshot_path);
    }

    GLOG_INFO("Entering editor main loop...");
    GLOG_INFO("Viewport controls: RMB drag look | WASD+QE move (hold RMB) | Shift sprint | Wheel speed | Close window to exit");

    utils::FrameLimiter frame_limiter;
    frame_limiter.set_target_fps(0); // 默认不限制帧率，让 GPU 全力跑

    bool wireframe_mode = false;
    double auto_close_timer = 0.0;
    double play_mode_test_timer = 0.0;
    bool play_mode_test_entered = false;

    // Viewport 面板尺寸 → 渲染目标尺寸：防抖 0.15s，避免拖动 dock 分隔条时
    // 每帧 pause/resume 渲染线程造成的卡顿。
    int pending_vw = 0, pending_vh = 0;
    int applied_vw = 0, applied_vh = 0;
    float viewport_resize_timer = 0.0f;
    constexpr float k_resize_debounce = 0.15f;

    while (!window.should_close()) {
        frame_limiter.begin_frame();
        window.update_frame_stats();

        if (auto_close_seconds > 0.0f) {
            auto_close_timer += window.delta_time();
            if (auto_close_timer >= static_cast<double>(auto_close_seconds)) {
                GLOG_INFO("Auto-close after {} seconds", auto_close_seconds);
                window.request_close();
                break;
            }
        }

        // Play Mode CI 测试：第 1 秒进入，第 3 秒退出
        if (test_play_mode) {
            play_mode_test_timer += window.delta_time();
            if (!play_mode_test_entered && play_mode_test_timer >= 1.0) {
                play_mode_test_entered = true;
                enter_play_mode();
            }
            if (play_mode_test_entered && play_mode_test_timer >= 3.0 && play_mode_active) {
                exit_play_mode();
            }
        }

        // 场景热重载计时器（实际重载在 present 之后执行）
        scene_reload_timer += window.delta_time();

        input.update(&window);

        // F1 切换线框模式（调试用，仅 OpenGL）
        if (!is_vulkan && input.is_key_pressed(GLFW_KEY_F1)) {
            wireframe_mode = !wireframe_mode;
            render_ctx.push_command([wireframe_mode](render::IRenderBackend*) {
                glPolygonMode(GL_FRONT_AND_BACK, wireframe_mode ? GL_LINE : GL_FILL);
            });
            GLOG_INFO("Wireframe mode: {}", wireframe_mode ? "ON" : "OFF");
        }

        float dt = static_cast<float>(window.delta_time());
        if (dt > 0.1f) dt = 0.1f; // 防卡顿

        // 编辑器相机：只在 Viewport 面板悬停且 gizmo 未激活时响应输入
        editor_camera.update(dt, viewport_panel->hovered() && !viewport_panel->gizmo_active());

        // 驱动 ECS 系统（物理、动画、游戏逻辑等）
        world.update(dt);

        // -------------------------------------------------------------------
        // Viewport 尺寸同步（上一帧面板尺寸，防抖后应用）
        // -------------------------------------------------------------------
        {
            const int cur_vw = static_cast<int>(viewport_panel->content_width());
            const int cur_vh = static_cast<int>(viewport_panel->content_height());
            if (cur_vw != pending_vw || cur_vh != pending_vh) {
                pending_vw = cur_vw;
                pending_vh = cur_vh;
                viewport_resize_timer = 0.0f;
            }
            viewport_resize_timer += dt;
            if (!is_vulkan && pending_vw >= 4 && pending_vh >= 4 &&
                (pending_vw != applied_vw || pending_vh != applied_vh) &&
                viewport_resize_timer >= k_resize_debounce) {
                render_ctx.pause_render_thread();
                if (pipeline.resize_render_targets(pending_vw, pending_vh)) {
                    applied_vw = pending_vw;
                    applied_vh = pending_vh;
                }
                render_ctx.resume_render_thread();
            }
        }

        int w = 0, h = 0;
        window.get_size(w, h);

        // 场景渲染分辨率：OpenGL 跟随 Viewport 面板，Vulkan 跟随窗口
        const int render_w = (!is_vulkan && applied_vw >= 4) ? applied_vw : w;
        const int render_h = (!is_vulkan && applied_vh >= 4) ? applied_vh : h;
        const float aspect = (render_w > 0 && render_h > 0)
                                 ? static_cast<float>(render_w) / static_cast<float>(render_h)
                                 : 1.0f;
        camera.set_aspect(aspect);

        render_ctx.set_viewport(0, 0, w, h);

        // 将 Camera 组件参数（fov/near/far）同步到编辑器相机，
        // 并把编辑器相机的位置/朝向写回 MainCamera 组件
        if (world.scene()) {
            apply_camera_component_to_global(*world.scene(), camera);
            sync_active_camera_to_scene(*world.scene(), camera);
        }

        // 设置渲染管线相机、灯光与视口
        pipeline.set_camera(camera);
        pipeline.set_lights(world.scene() ? collect_lights(*world.scene())
                                          : std::vector<render::RenderPipeline::Light>{});
        pipeline.set_viewport(render_w, render_h);

        // -------------------------------------------------------------------
        // 3D + 2D 场景渲染由 ECS World 驱动
        // -------------------------------------------------------------------
        if (fps_label) {
            fps_label->text = std::format("FPS: {:.1f}", window.fps());
        }

        if (renderer2d) {
            renderer2d->begin_frame(static_cast<float>(w), static_cast<float>(h));
        }

        world.render(render_ctx);

        if (renderer2d) {
            renderer2d->end_frame();
        }

        // 3D/2D 场景已写入视口离屏 FBO，ImGui 必须回到默认 framebuffer 绘制，
        // 否则 swap buffers 时屏幕显示的是未初始化的默认 framebuffer（全黑）。
        render_ctx.set_framebuffer(render::RHIFramebufferHandle{});
        render_ctx.clear(0.1f, 0.1f, 0.1f, 1.0f);

        // -------------------------------------------------------------------
        // 编辑器 ImGui UI：DockSpace + 面板 + File 菜单弹窗 + 点选拾取
        // -------------------------------------------------------------------
        hierarchy_panel->set_scene(world.scene());
        inspector_panel->set_target(hierarchy_panel->selected_entity());

        imgui.begin_frame();
        ImGuizmo::BeginFrame();

        panel_manager.show();

        // 设置窗口（File > Settings）
        settings_window.draw(project_root.string(), editor_settings);

        // Ctrl+S 保存 / Ctrl+P 切换 Play Mode（ImGui 正处理键盘输入时不抢快捷键）
        if (ImGui::GetIO().KeyCtrl && !ImGui::GetIO().WantCaptureKeyboard) {
            if (ImGui::IsKeyPressed(ImGuiKey_S, false)) {
                save_scene(scene_path);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_P, false)) {
                toggle_play_mode();
            }
        }

        // Save Scene As 弹窗
        if (save_as_popup_requested) {
            ImGui::OpenPopup("SaveSceneAs");
            save_as_popup_requested = false;
        }
        if (ImGui::BeginPopupModal((std::string(editor::tr("dialog.save_scene_as")) + "###SaveSceneAs").c_str(),
                                   nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("%s", editor::tr("dialog.save_scene_to"));
            ImGui::InputText("##save_as_path", save_as_buf, sizeof(save_as_buf));
            if (ImGui::Button(editor::tr("dialog.save")) ||
                (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Enter, false))) {
                save_scene(save_as_buf);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(editor::tr("dialog.cancel"))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Open Scene 弹窗
        if (open_popup_requested) {
            ImGui::OpenPopup("OpenScene");
            open_popup_requested = false;
        }
        if (ImGui::BeginPopupModal((std::string(editor::tr("dialog.open_scene")) + "###OpenScene").c_str(),
                                   nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("%s", editor::tr("dialog.open_scene_from"));
            ImGui::InputText("##open_path", open_buf, sizeof(open_buf));
            if (ImGui::Button(editor::tr("dialog.open")) ||
                (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Enter, false))) {
                open_scene(open_buf);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(editor::tr("dialog.cancel"))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // 点选拾取：Viewport 左击 UV → NDC → 世界射线 → AABB 求交（非 Play 状态才生效）
        ImVec2 pick_uv;
        if (!play_mode_active && world.scene() && viewport_panel->take_pick_click(pick_uv)) {
            const float ndc_x = pick_uv.x * 2.0f - 1.0f;
            const float ndc_y = 1.0f - pick_uv.y * 2.0f; // ImGui y 向下 → NDC y 向上
            const math::Matrix4f inv_vp =
                (camera.get_projection_matrix() * camera.get_view_matrix()).inverse();
            const math::Ray ray = math::screen_ndc_to_ray(ndc_x, ndc_y, inv_vp);
            if (scene::Entity* hit = pick_entity(*world.scene(), ray)) {
                hierarchy_panel->select(hit->uuid());
                GLOG_INFO("Picked entity '{}'", hit->name());
            } else {
                hierarchy_panel->clear_selection();
            }
        }

        imgui.end_frame([&](ImDrawData* draw_data, std::shared_ptr<std::promise<void>> sync_promise) {
            // 深拷贝 ImDrawData，避免无限制帧率下主线程继续 NewFrame 覆盖 draw data
            auto owned_draw_data = imgui.clone_draw_data(draw_data);
            render_ctx.push_command([owned_draw_data, &imgui, sync_promise](render::IRenderBackend*) {
                imgui.render_draw_data(owned_draw_data.get());
                sync_promise->set_value();
            });
        });

        render_ctx.present();

        // -------------------------------------------------------------------
        // 场景热重载：必须在 present 之后执行，
        // 这样 command buffer 为空，pause 不会丢失未提交的渲染命令。
        // -------------------------------------------------------------------
        if (!is_vulkan && scene_reload_timer >= 1.0) {
            scene_reload_timer = 0.0;
            auto new_time = get_scene_write_time(scene_path);
            if (new_time != std::filesystem::file_time_type::min() &&
                new_time != scene_last_write) {
                auto reloaded = try_reload_scene(scene_path, world.detach_scene(),
                                                 new_time, scene_last_write);
                hierarchy_panel->clear_selection();

                if (reloaded) {
                    render_ctx.pause_render_thread();

                    // 确保物理演示场景有地面和可下落的 cube
                    ensure_physics_demo_entities(*reloaded);

                    upload_scene_meshes(*reloaded, render_ctx);
                    ensure_scene_defaults(*reloaded, camera);
                    world.attach_scene(std::move(reloaded));
                    render_ctx.resume_render_thread();

                    fps_label = nullptr;
                    scene::Entity* fps_entity = world.scene()->find_entity_by_name("FPS_Label");
                    if (fps_entity) {
                        fps_label = fps_entity->get_component<components::d2::text::Label>();
                    }
                }
            }
        }

        window.poll_events();

        // 用户点了关闭按钮后立即退出，不再跑下一帧
        if (window.should_close() || window.close_requested()) {
            GLOG_INFO("Editor loop: close requested, breaking");
            frame_limiter.end_frame();
            break;
        }

        // CPU 侧帧率限制放在 poll_events 之后，这样 glfwPollEvents 的开销也被
        // 算进目标帧时间，实际 FPS 才会接近设定值。
        frame_limiter.end_frame();
    }

    GLOG_INFO("Editor loop exited. FPS: {:.1f}", window.fps());

    // 退出时保存场景
    if (world.scene()) {
        scene::SceneSerializer::save_to_file(*world.scene(), scene_path);
        GLOG_INFO("Scene saved to '{}'", scene_path);
    }

    // 退出顺序：
    // 1) 先暂停渲染线程并等待 GPU 空闲，防止后续资源销毁时命令仍在执行；
    // 2) 停止 ECS System；
    // 3) 析构 Scene Entity/Component；
    // 4) 销毁 2D / ImGui / Pipeline；
    // 5) 关闭 RenderContext。
    render_ctx.pause_render_thread_keep_cmdbuffer();
    world.shutdown();
    {
        auto scene = world.detach_scene();
        scene.reset();
    }

    if (renderer2d) {
        renderer2d->shutdown();
    }
    imgui.shutdown();
    pipeline.shutdown();
    render_ctx.shutdown();

    // 先显式销毁窗口，再 glfwTerminate；顺序反过来可能导致延迟或异常。
    window.destroy();
    platform::Window::shutdown_sdk();
    utils::glog_shutdown();

    std::cout << "All systems nominal." << std::endl;
    return 0;
}

} // namespace gryce_engine::editor
