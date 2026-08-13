// 2dDemo - 2D 平台跳跃游戏入口（Gryce Engine 示例）。
//
// 本文件只做"游戏宿主"：初始化 Core/物理/窗口/渲染、每帧同步输入并驱动
// 引擎循环。全部玩法逻辑（角色控制、枪械、敌人、昼夜、过关条件、HUD）都在
// Lua 脚本里（examples/2dDemo/scripts/*.lua），关卡内容在编辑器可加载的
// .gesc 场景中（scenes/level_*.gesc）。同样的脚本在 WPF 编辑器 Play 模式、
// 本 exe、以及 grycegc 打包产物（GryceGame 模板）中都能运行。

#include "GryceCore/core_api.h"
#include "GryceCore/scene_api.h"
#include "GrycePlatform/window_api.h"
#include "GrycePlatform/input_api.h"
#include "GryceRenderer/render_api.h"
#include "GryceRenderer/viewport_api.h"
#include "GrycePhysics/physics_api.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <nlohmann/json.hpp>

namespace {

// 项目根定位：先看 exe 所在目录（打包产物），再回退到引擎仓库 examples/<exe>。
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
    if (exe_path.empty()) return std::filesystem::current_path();

    std::filesystem::path dir = exe_path.parent_path();
    if (std::filesystem::exists(dir / "project.gryce")) {
        return dir;
    }
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

// --level N（1 基）→ levels.json 里的场景路径
std::string resolve_level_scene(const std::string& root, int level) {
    if (level <= 0) return "";
    std::ifstream in(root + "/levels.json");
    if (!in) return "";
    try {
        nlohmann::json j;
        in >> j;
        const auto& arr = j.value("levels", nlohmann::json::array());
        if (level > static_cast<int>(arr.size())) return "";
        return arr[level - 1].value("scene", "");
    } catch (const std::exception&) {
        return "";
    }
}

void enter_play_mode() {
    GCommand cmd{};
    cmd.type = ECMD_PLAY_MODE;
    cmd.seq = 0;
    GCore_PushCommand(&cmd);
}

} // namespace

int main(int argc, char* argv[]) {
#ifdef _WIN32
    // DPI-aware：让 GLFW 窗口/帧缓冲使用真实物理像素，避免 DWM 虚拟化缩放
    // 导致 2D 视图矩阵与屏幕映射不一致（编辑器为 WPF，天然 DPI-aware）。
    {
        using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(void*);
        if (HMODULE user32 = ::GetModuleHandleW(L"user32.dll")) {
            auto fn = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
                ::GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
            if (fn) {
                fn(reinterpret_cast<void*>(-4));  // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
            }
        }
    }
#endif

    std::string project;
    std::string scene_override;
    float auto_close_seconds = 0.0f;
    int width = 1280;
    int height = 720;

    for (int i = 1; i < argc; ++i) {
        auto need = [&](const char* opt) -> const char* {
            return (i + 1 < argc) ? argv[++i] : nullptr;
        };
        if (std::strcmp(argv[i], "--project") == 0) {
            if (const char* v = need("--project")) project = v;
        } else if (std::strcmp(argv[i], "--scene") == 0) {
            if (const char* v = need("--scene")) scene_override = v;
        } else if (std::strcmp(argv[i], "--level") == 0) {
            if (const char* v = need("--level")) {
                scene_override = resolve_level_scene(find_project_root().string(),
                                                     std::atoi(v));
                if (scene_override.empty()) {
                    std::fprintf(stderr, "[2dDemo] invalid --level %s\n", v);
                    return 1;
                }
            }
        } else if (std::strcmp(argv[i], "--auto-close") == 0) {
            if (const char* v = need("--auto-close")) {
                auto_close_seconds = static_cast<float>(std::atof(v));
            }
        } else if (std::strcmp(argv[i], "--w") == 0) {
            if (const char* v = need("--w")) width = std::atoi(v);
        } else if (std::strcmp(argv[i], "--h") == 0) {
            if (const char* v = need("--h")) height = std::atoi(v);
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::printf(
                "2dDemo (GryceSRT 游戏宿主)\n"
                "  --project <dir>   项目根（默认自动定位）\n"
                "  --scene <res:...> 覆盖起始场景\n"
                "  --level <N>       从 levels.json 的第 N 关开始\n"
                "  --auto-close <s>  运行 s 秒后自动退出（CI/测试用）\n"
                "  --w/--h <px>      窗口尺寸\n");
            return 0;
        }
    }

    if (project.empty()) {
        project = find_project_root().string();
    }
    std::printf("[2dDemo] project root = %s\n", project.c_str());

    // Core 初始化（自动加载主场景；--scene/--level 覆盖时关闭自动加载）
    GCore_SetAutoLoadMainScene(scene_override.empty());
    GCoreInitDesc core_desc{};
    core_desc.version = sizeof(GCoreInitDesc);
    core_desc.project_root = project.c_str();
    core_desc.enable_reflection = true;
    if (GCore_Init(&core_desc) != 0) {
        std::fprintf(stderr, "[2dDemo] GCore_Init failed\n");
        return 1;
    }

    if (void* world = GCore_GetInternalWorldPtr()) {
        if (GPhysics_Init(GPHYSICS_BACKEND_JOLT) == 0) {
            GPhysics_AttachSystems(world);
        }
    }

    if (GWindow_Create("Gryce Engine - 2dDemo", width, height,
                       GWINDOW_MODE_WINDOWED) != 0) {
        std::fprintf(stderr, "[2dDemo] GWindow_Create failed\n");
        GCore_Shutdown();
        return 1;
    }
    // 实际窗口/帧缓冲尺寸（高 DPI 缩放下可能与请求值不同）
    int win_w = 0, win_h = 0;
    GWindow_GetSize(&win_w, &win_h);
    if (win_w > 0 && win_h > 0) {
        width = win_w;
        height = win_h;
        std::printf("[2dDemo] actual window size: %dx%d\n", win_w, win_h);
    }

    GRenderInitDesc render_desc{};
    render_desc.version = sizeof(GRenderInitDesc);
    render_desc.native_window = GWindow_GetRenderHandle();
    render_desc.api = GRYCE_RENDER_API_OPENGL;
    render_desc.viewport_w = width;
    render_desc.viewport_h = height;
    render_desc.sync_mode = true;
    if (GRender_Init(&render_desc) != 0) {
        std::fprintf(stderr, "[2dDemo] GRender_Init failed\n");
        GWindow_Destroy();
        GCore_Shutdown();
        return 1;
    }
    // 2D 游戏：纯 2D 画布路径（与编辑器 2D 模式一致），跳过 3D 管线
    GRender_SetScene2D(true);
    // 视口与真实窗口尺寸对齐（编辑器在每次尺寸变化时同样调用）
    GViewport_SetSize(width, height);

    if (!scene_override.empty()) {
        if (GScene_Load(scene_override.c_str()) != 0) {
            std::fprintf(stderr, "[2dDemo] failed to load scene %s\n",
                         scene_override.c_str());
        }
    }
    enter_play_mode();

    auto last = std::chrono::steady_clock::now();
    double auto_close_timer = 0.0;
    int last_w = width, last_h = height;
    while (!GWindow_ShouldClose()) {
        GWindow_PollEvents();
        GInput_SyncToCore();

        int cur_w = 0, cur_h = 0;
        GWindow_GetSize(&cur_w, &cur_h);
        if (cur_w > 0 && cur_h > 0 && (cur_w != last_w || cur_h != last_h)) {
            last_w = cur_w;
            last_h = cur_h;
            GViewport_SetSize(cur_w, cur_h);
        }

        const auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        if (dt < 0.0f || dt > 0.05f) dt = 0.016f;

        if (auto_close_seconds > 0.0f) {
            auto_close_timer += dt;
            if (auto_close_timer >= static_cast<double>(auto_close_seconds)) {
                std::printf("[2dDemo] auto-close after %.1f seconds\n",
                            auto_close_seconds);
                break;
            }
        }

        GCore_BeginFrame(dt);
        GRender_BeginFrame();
        GRender_RenderWorld();
        GRender_EndFrame();

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    GRender_Shutdown();
    GWindow_Destroy();
    GPhysics_Shutdown();
    GCore_Shutdown();
    std::printf("[2dDemo] exited\n");
    return 0;
}
