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

#include <GL/glew.h>
#include <GLFW/glfw3.h>

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <windows.h>
#endif

#include <imgui.h>

#include "platform/window.h"
#include "platform/input.h"
#include "platform/cursor.h"
#include "render/render_context.h"
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
#include "math/camera.h"
#include "utils/glog/glog_lib.h"
#include "utils/frame_limiter.h"
#include "ecs/world.h"
#include "ecs/systems/physics_system_3d.h"
#include "ecs/systems/render_system_2d.h"
#include "ecs/systems/render_system_3d.h"
#include "render/render_pipeline.h"
#include "ui/debug_panel.h"

using namespace gryce_engine;

// ---------------------------------------------------------------------------
// 生成测试纹理（棋盘格 BMP，top-down，24bpp）
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
    // 从可执行文件目录向上查找包含 CMakeLists.txt 的目录
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        if (std::filesystem::exists(dir / "CMakeLists.txt")) {
            return dir;
        }
        dir = dir.parent_path();
    }
    return std::filesystem::current_path();
}

// ---------------------------------------------------------------------------
// 场景辅助函数
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

    // 将全局 FPS 摄像机的位置/朝向写回组件，便于保存场景。
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
        lights.push_back({ math::Vector3f(0.0f, -1.0f, 0.0f), math::Vector3f::one(), 1.0f });
    }
    return lights;
}

// ---------------------------------------------------------------------------
// Entry Point
// ---------------------------------------------------------------------------
int main(int argc, char* argv[])
{
    std::cout << "Gryce Engine v0.1.0 - Scene Demo" << std::endl;

    // 解析命令行参数
    render::RenderAPI selected_api = render::RenderAPI::OpenGL;
    bool screenshot_mode = false;
    bool vulkan_validation = false; // 默认关闭 validation，需要时通过 --vulkan-validation 开启
    float auto_close_seconds = 0.0f; // --auto-close N：运行 N 秒后自动关闭，用于 CI/关机测试
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--vulkan") == 0) {
            selected_api = render::RenderAPI::Vulkan;
        } else if (std::strcmp(argv[i], "--screenshot") == 0) {
            screenshot_mode = true;
        } else if (std::strcmp(argv[i], "--vulkan-validation") == 0) {
            vulkan_validation = true;
        } else if (std::strcmp(argv[i], "--auto-close") == 0 && i + 1 < argc) {
            auto_close_seconds = static_cast<float>(std::atof(argv[++i]));
        }
    }

    utils::glog_initialize();
    utils::GLog::instance().set_min_level(utils::LogLevel::Info);

    // 设置项目根目录（自动从可执行文件位置向上查找）
    resources::Project::instance().set_root(find_project_root().string());
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
    platform::Window window("Gryce Engine - Cube + Camera + Scene", 1280, 720,
                            platform::WindowMode::Windowed, window_ctx);
    if (!window.is_valid()) {
        GLOG_ERROR("Failed to create window");
        platform::Window::shutdown_sdk();
        return -1;
    }
    if (selected_api == render::RenderAPI::OpenGL) {
        window.set_vsync(false);
    }

    // 加载自定义鼠标光标（可选）：有贴图则使用，没有则在窗口焦点时隐藏
    platform::Cursor custom_cursor;
    std::string cursor_path = resources::ResourcePath::resolve("res:/textures/cursor.png");
    if (std::filesystem::exists(cursor_path)) {
        if (!custom_cursor.load_from_file(cursor_path, 0, 0)) {
            GLOG_WARN("Failed to load custom cursor from '{}'", cursor_path);
        }
    } else {
        GLOG_INFO("No custom cursor texture found at '{}', cursor will be hidden while focused", cursor_path);
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
    editor::ui::DebugPanel debug_panel;
    editor::ui::ModelLoaderPanel model_loader_panel;

    // 输入 + 摄像机
    platform::InputManager input;
    input.update(&window);          // 先让 InputManager 持有 window，后续 set_mouse_locked 才能真正设置光标
    input.set_mouse_locked(false);  // 启动时不锁定鼠标，按 Tab 进入 FPS 模式，ESC 解锁

    math::Camera camera;
    camera.set_position(math::Vector3f(0.0f, 0.0f, 5.0f));

    // 加载或创建场景（必须在 render_ctx.start() 之前，因为上传网格需要主线程 GL context）
    const std::string scene_path = "res:/scenes/main.gesc";
    std::unique_ptr<scene::Scene> current_scene = scene::SceneSerializer::load_from_file(scene_path);
    if (!current_scene) {
        GLOG_INFO("No existing scene found at '{}', creating demo scene", scene_path);
        current_scene = create_demo_scene(1280.0f, 720.0f);
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

    // 创建渲染管线（必须在 start() 之前，主线程持有 GL context）
    render::RenderPipeline pipeline;
    if (!pipeline.init(&render_ctx, "res:/shaders")) {
        GLOG_ERROR("Failed to initialize render pipeline");
        render_ctx.shutdown();
        platform::Window::shutdown_sdk();
        return -1;
    }

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

    // 启动渲染线程（此后主线程不再持有 GL context）
    render_ctx.start();

    if (screenshot_mode) {
        const std::string screenshot_path = is_vulkan
                                                ? "D:/Gryce-Engine/screenshot_vulkan.bmp"
                                                : "D:/Gryce-Engine/screenshot_opengl.bmp";
        render_ctx.request_screenshot(screenshot_path);
        GLOG_INFO("Screenshot requested on next frame: '{}'", screenshot_path);
    }

    GLOG_INFO("Entering render loop (cube demo)...");
    GLOG_INFO("Controls: WASD move | Space up | Ctrl down | Shift sprint | Mouse look | Tab toggle mouse | ESC unlock mouse | Close window to exit");

    utils::FrameLimiter frame_limiter;
    frame_limiter.set_target_fps(0); // 默认不限制帧率，让 GPU 全力跑

    bool wireframe_mode = false;
    double auto_close_timer = 0.0;

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

        // 场景热重载计时器（实际重载在 present 之后执行）
        scene_reload_timer += window.delta_time();

        input.update(&window);

        // ESC 解锁鼠标（关闭窗口用标题栏 X 按钮）
        if (input.is_key_pressed(GLFW_KEY_ESCAPE)) {
            input.set_mouse_locked(false);
        }

        // Tab 切换鼠标锁定
        if (input.is_key_pressed(GLFW_KEY_TAB)) {
            input.set_mouse_locked(!input.is_mouse_locked());
        }

        // F1 切换线框模式（调试用，仅 OpenGL）
        if (!is_vulkan && input.is_key_pressed(GLFW_KEY_F1)) {
            wireframe_mode = !wireframe_mode;
            render_ctx.push_command([wireframe_mode](render::IRenderBackend*) {
                glPolygonMode(GL_FRONT_AND_BACK, wireframe_mode ? GL_LINE : GL_FILL);
            });
            GLOG_INFO("Wireframe mode: {}", wireframe_mode ? "ON" : "OFF");
        }

        // 根据焦点状态显式同步光标：
        // - 有焦点 + 鼠标锁定（FPS 模式）：DISABLED，隐藏并捕获鼠标
        // - 有焦点 + 未锁定 + 有自定义光标贴图：显示自定义光标
        // - 有焦点 + 未锁定 + 无贴图：隐藏光标（默认行为）
        // - 无焦点：恢复系统默认光标，方便操作其他窗口
        if (window.has_focus()) {
            if (input.is_mouse_locked()) {
                window.set_cursor_disabled(true);
            } else {
                window.set_cursor_disabled(false);
                if (custom_cursor.is_valid()) {
                    window.set_cursor(&custom_cursor);
                    window.set_cursor_visible(true);
                } else {
                    window.set_cursor(nullptr);
                    window.set_cursor_visible(false);
                }
            }
        } else {
            window.set_cursor_disabled(false);
            window.set_cursor(nullptr);
            window.set_cursor_visible(true);
        }

        // 更新摄像机：
        // - 键盘移动只在有焦点时生效；
        // - 鼠标控制视角只在“鼠标锁定（FPS 模式）且窗口有焦点”时生效，
        //   按 ESC 解锁后不再影响摄像机。
        float dt = static_cast<float>(window.delta_time());
        if (dt > 0.1f) dt = 0.1f; // 防卡顿

        bool mouse_look = window.has_focus() && input.is_mouse_locked();
        // GLFW 鼠标 Y 正向为向下移动；标准 FPS 中鼠标上移应抬头（pitch 负向），
        // 因此默认取反，unchecked 状态即为标准 FPS。
        float mouse_dy = mouse_look ? -static_cast<float>(input.mouse_delta_y()) : 0.0f;
        if (debug_panel.invert_mouse_y()) mouse_dy = -mouse_dy;

        // 标准 FPS：Space 上升，Left Ctrl 下降；勾选 Swap 时交换。
        bool move_up   = input.is_key_held(GLFW_KEY_SPACE);
        bool move_down = input.is_key_held(GLFW_KEY_LEFT_CONTROL);
        if (debug_panel.swap_space_ctrl()) {
            std::swap(move_up, move_down);
        }

        camera.update(dt,
                      input.is_key_held(GLFW_KEY_W),
                      input.is_key_held(GLFW_KEY_S),
                      input.is_key_held(GLFW_KEY_A),
                      input.is_key_held(GLFW_KEY_D),
                      move_up,
                      move_down,
                      input.is_key_held(GLFW_KEY_LEFT_SHIFT),
                      mouse_look ? static_cast<float>(input.mouse_delta_x()) : 0.0f,
                      mouse_dy);

        // 驱动 ECS 系统（物理、动画、游戏逻辑等）
        world.update(dt);

        pipeline.set_cull_disabled(debug_panel.disable_cull());

        // 矩阵计算
        int w = 0, h = 0;
        window.get_size(w, h);
        float aspect = (w > 0 && h > 0) ? static_cast<float>(w) / static_cast<float>(h) : 1.0f;
        camera.set_aspect(aspect);

        // 渲染命令
        render_ctx.set_viewport(0, 0, w, h);

        // 将 Camera 组件参数同步到全局摄像机，并把全局摄像机的位置/朝向写回组件
        apply_camera_component_to_global(*world.scene(), camera);
        sync_active_camera_to_scene(*world.scene(), camera);

        // 设置渲染管线相机、灯光与视口
        pipeline.set_camera(camera);
        pipeline.set_lights(collect_lights(*world.scene()));
        pipeline.set_viewport(w, h);

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

        // -------------------------------------------------------------------
        // ImGui 调试 UI
        // -------------------------------------------------------------------
        bool model_loaded = false;
        imgui.begin_frame();
        debug_panel.show(&window, world.scene(), &camera, &frame_limiter, &render_ctx, &pipeline);
        model_loaded = model_loader_panel.show(world.scene());

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
        // 场景热重载 / 动态模型加载：必须在 present 之后执行，
        // 这样 command buffer 为空，pause 不会丢失未提交的渲染命令。
        // -------------------------------------------------------------------
        if (!is_vulkan && scene_reload_timer >= 1.0) {
            scene_reload_timer = 0.0;
            auto new_time = get_scene_write_time(scene_path);
            if (new_time != std::filesystem::file_time_type::min() &&
                new_time != scene_last_write) {
                auto reloaded = try_reload_scene(scene_path, world.detach_scene(),
                                                 new_time, scene_last_write);
                debug_panel.clear_selection();

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

        if (!is_vulkan && model_loaded && world.scene()) {
            render_ctx.pause_render_thread();
            upload_scene_meshes(*world.scene(), render_ctx);
            render_ctx.resume_render_thread();
        }

        window.poll_events();

        // 用户点了关闭按钮后立即退出，不再跑下一帧
        if (window.should_close() || window.close_requested()) {
            GLOG_INFO("Render loop: close requested, breaking");
            frame_limiter.end_frame();
            break;
        }

        // 无限制帧率时不再 wait_for_idle：
        // 1) ImDrawData 已深拷贝到渲染命令中，不会被 NewFrame 覆盖；
        // 2) RenderCommandBuffer 的三缓冲会在 3 帧占满时自然阻塞主线程，避免命令无限堆积。
        // 有限制时由 sleep 自然 pacing。

        // CPU 侧帧率限制放在 poll_events 之后，这样 glfwPollEvents 的开销也被
        // 算进目标帧时间，实际 FPS 才会接近设定值。
        frame_limiter.end_frame();
    }

    GLOG_INFO("Render loop exited. FPS: {:.1f}", window.fps());

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
