#include "app_launcher.h"

#include <iostream>
#include <cstring>
#include <cstdlib>
#include <filesystem>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#include <climits>
#endif

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <imgui.h>

#include "platform/window.h"
#include "render/opengl/imgui_renderer.h"
#include "components/2d/label.h"
#include "components/2d/basic_rect.h"
#include "components/component_factory.h"
#include "ecs/systems/render_system_2d.h"
#include "ecs/systems/render_system_3d.h"
#include "resources/project.h"
#include "utils/glog/glog_lib.h"
#include "utils/frame_limiter.h"

namespace gryce_engine::examples {

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
        std::string exe_name = exe_path.stem().string();
        std::filesystem::path candidate = engine_root / "examples" / exe_name;
        if (std::filesystem::exists(candidate / "project.gryce")) {
            return candidate;
        }
    }
    return std::filesystem::current_path();
}

namespace {

struct DemoArgs {
    bool vulkan = false;
    bool vulkan_validation = false;
    float auto_close_seconds = 0.0f;
};

static DemoArgs parse_args(int argc, char* argv[]) {
    DemoArgs args;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--vulkan") == 0) {
            args.vulkan = true;
        } else if (std::strcmp(argv[i], "--vulkan-validation") == 0) {
            args.vulkan_validation = true;
        } else if (std::strcmp(argv[i], "--auto-close") == 0 && i + 1 < argc) {
            args.auto_close_seconds = static_cast<float>(std::atof(argv[++i]));
        }
    }
    return args;
}

static scene::Entity* create_fps_label(scene::Scene& scene) {
    scene::Entity* fps_bg = scene.create_entity("FPS_BG");
    fps_bg->transform()->position = math::Vector3f(5.0f, 5.0f, 0.0f);
    fps_bg->add_component<components::d2::basic_rect::ColorRect>(120.0f, 30.0f,
                                                                  render::Color(0.2f, 0.2f, 0.2f, 0.8f));

    scene::Entity* fps_label = scene.create_entity("FPS_Label");
    fps_label->transform()->position = math::Vector3f(15.0f, 23.0f, 0.0f);
    fps_label->add_component<components::d2::text::Label>("FPS: 0.0", 18.0f, render::Color::white());
    return fps_label;
}

} // namespace

int run_demo(DemoApp& app, int argc, char* argv[]) {
    DemoArgs args = parse_args(argc, argv);

    utils::glog_initialize();
    utils::GLog::instance().set_min_level(utils::LogLevel::Info);

    resources::Project::instance().set_root(find_project_root().string());
    components::register_builtin_components();

    if (!platform::Window::init_sdk()) {
        GLOG_ERROR("Failed to initialize GLFW");
        return -1;
    }

    platform::WindowContextType window_ctx = args.vulkan
                                                 ? platform::WindowContextType::NoApi
                                                 : platform::WindowContextType::OpenGL;
    platform::Window window(app.title(), 1280, 720,
                            platform::WindowMode::Windowed, window_ctx);
    if (!window.is_valid()) {
        GLOG_ERROR("Failed to create window");
        platform::Window::shutdown_sdk();
        return -1;
    }
    if (!args.vulkan) {
        window.set_vsync(false);
    }

    render::RenderContext render_ctx;
    render_ctx.set_validation_enabled(args.vulkan_validation);
    if (!render_ctx.init(window.native_handle(),
                         args.vulkan ? render::RenderAPI::Vulkan : render::RenderAPI::OpenGL)) {
        GLOG_ERROR("Failed to initialize render context");
        platform::Window::shutdown_sdk();
        return -1;
    }

    window.set_resize_callback([&](int w, int h) {
        render_ctx.set_viewport(0, 0, w, h);
    });

    std::unique_ptr<render::IRenderer2D> renderer2d = render_ctx.create_renderer2d();
    if (renderer2d) {
        renderer2d->init(&render_ctx);
    }

    render::ImGuiRenderer imgui;
    auto imgui_backend = render_ctx.create_imgui_backend();
    imgui.init(static_cast<GLFWwindow*>(window.native_handle()), std::move(imgui_backend));

    std::unique_ptr<render::RenderPipeline> pipeline;
    if (app.is_3d()) {
        pipeline = std::make_unique<render::RenderPipeline>();
        if (!pipeline->init(&render_ctx, "res:/shaders")) {
            GLOG_ERROR("Failed to initialize render pipeline");
            render_ctx.shutdown();
            platform::Window::shutdown_sdk();
            return -1;
        }
    }

    auto scene = std::make_unique<scene::Scene>("main");
    if (!app.init_scene(*scene, render_ctx)) {
        GLOG_ERROR("Demo scene initialization failed");
        render_ctx.shutdown();
        platform::Window::shutdown_sdk();
        return -1;
    }

    // 为所有 demo 提供 FPS 显示
    scene::Entity* fps_entity = create_fps_label(*scene);
    auto* fps_label = fps_entity->get_component<components::d2::text::Label>();

    ecs::World world;
    world.attach_scene(std::move(scene));
    app.register_systems(world, pipeline.get(), renderer2d.get());
    world.init();

    render_ctx.start();

    platform::InputManager input;
    input.update(&window);
    input.set_mouse_locked(false);

    utils::FrameLimiter frame_limiter;
    frame_limiter.set_target_fps(0);

    double auto_close_timer = 0.0;

    GLOG_INFO("Entering render loop: {}", app.title());

    while (!window.should_close()) {
        frame_limiter.begin_frame();
        window.update_frame_stats();

        if (args.auto_close_seconds > 0.0f) {
            auto_close_timer += window.delta_time();
            if (auto_close_timer >= static_cast<double>(args.auto_close_seconds)) {
                window.request_close();
                break;
            }
        }

        input.update(&window);

        if (input.is_key_pressed(GLFW_KEY_ESCAPE)) {
            input.set_mouse_locked(false);
            window.request_close();
        }

        float dt = static_cast<float>(window.delta_time());
        if (dt > 0.1f) dt = 0.1f;

        app.update(dt, input, *world.scene());
        world.update(dt);

        int w = 0, h = 0;
        window.get_size(w, h);
        render_ctx.set_viewport(0, 0, w, h);

        if (fps_label) {
            fps_label->text = std::format("FPS: {:.1f}", window.fps());
        }

        if (renderer2d) {
            renderer2d->begin_frame(static_cast<float>(w), static_cast<float>(h));
            renderer2d->set_camera(math::Vector2f(static_cast<float>(w) * 0.5f,
                                                  static_cast<float>(h) * 0.5f), 1.0f);
        }

        world.render(render_ctx);

        if (renderer2d) {
            renderer2d->end_frame();
        }

        if (app.use_imgui()) {
            imgui.begin_frame();
            app.render_ui(*world.scene());
            imgui.end_frame([&](ImDrawData* draw_data, std::shared_ptr<std::promise<void>> sync_promise) {
                auto owned_draw_data = imgui.clone_draw_data(draw_data);
                render_ctx.push_command([owned_draw_data, &imgui, sync_promise](render::IRenderBackend*) {
                    imgui.render_draw_data(owned_draw_data.get());
                    sync_promise->set_value();
                });
            });
        }

        render_ctx.present();
        window.poll_events();
        frame_limiter.end_frame();
    }

    GLOG_INFO("Render loop exited. FPS: {:.1f}", window.fps());

    render_ctx.pause_render_thread_keep_cmdbuffer();
    world.shutdown();
    {
        auto detached = world.detach_scene();
        detached.reset();
    }

    if (renderer2d) {
        renderer2d->shutdown();
    }
    imgui.shutdown();
    if (pipeline) {
        pipeline->shutdown();
    }
    render_ctx.shutdown();
    window.destroy();
    platform::Window::shutdown_sdk();
    utils::glog_shutdown();

    return 0;
}

} // namespace gryce_engine::examples
