// FPSDemo - 3D FPS 游戏入口（Gryce Engine 示例）。
//
// 本文件只做"游戏宿主"：初始化 Core/物理(Jolt)/窗口/渲染、每帧同步输入并
// 驱动引擎循环。全部玩法逻辑（第一人称移动、鼠标视角、射击、敌人 AI、胜负、
// HUD、音效/音乐）都在 Lua 脚本里（examples/FPSDemo/scripts/*.lua），关卡内容
// 在编辑器可加载的 .gesc 场景中（scenes/main.gesc）。同样的脚本在 WPF 编辑器
// Play 模式、本 exe、以及 grycegc 打包产物（GryceGame 模板）中都能运行。

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
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#include <climits>
#endif

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
    // 导致视口矩阵与屏幕映射不一致（编辑器为 WPF，天然 DPI-aware）。
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
                "FPSDemo (GryceSRT 游戏宿主)\n"
                "  --project <dir>   项目根（默认自动定位）\n"
                "  --scene <res:...> 覆盖起始场景\n"
                "  --auto-close <s>  运行 s 秒后自动退出（CI/测试用）\n"
                "  --w/--h <px>      窗口尺寸\n");
            return 0;
        }
    }

    if (project.empty()) {
        project = find_project_root().string();
    }
    std::printf("[FPSDemo] project root = %s\n", project.c_str());

    // Core 初始化（自动加载主场景；--scene 覆盖时关闭自动加载）
    GCore_SetAutoLoadMainScene(scene_override.empty());
    GCoreInitDesc core_desc{};
    core_desc.version = sizeof(GCoreInitDesc);
    core_desc.project_root = project.c_str();
    core_desc.enable_reflection = true;
    if (GCore_Init(&core_desc) != 0) {
        std::fprintf(stderr, "[FPSDemo] GCore_Init failed\n");
        return 1;
    }

    if (void* world = GCore_GetInternalWorldPtr()) {
        if (GPhysics_Init(GPHYSICS_BACKEND_JOLT) == 0) {
            GPhysics_AttachSystems(world);
        }
    }

    if (GWindow_Create("Gryce Engine - FPS Demo", width, height,
                       GWINDOW_MODE_WINDOWED) != 0) {
        std::fprintf(stderr, "[FPSDemo] GWindow_Create failed\n");
        GCore_Shutdown();
        return 1;
    }
    // 实际窗口/帧缓冲尺寸（高 DPI 缩放下可能与请求值不同）
    int win_w = 0, win_h = 0;
    GWindow_GetSize(&win_w, &win_h);
    if (win_w > 0 && win_h > 0) {
        width = win_w;
        height = win_h;
        std::printf("[FPSDemo] actual window size: %dx%d\n", win_w, win_h);
    }

    GRenderInitDesc render_desc{};
    render_desc.version = sizeof(GRenderInitDesc);
    render_desc.native_window = GWindow_GetRenderHandle();
    render_desc.api = GRYCE_RENDER_API_OPENGL;
    render_desc.viewport_w = width;
    render_desc.viewport_h = height;
    render_desc.sync_mode = true;
    if (GRender_Init(&render_desc) != 0) {
        std::fprintf(stderr, "[FPSDemo] GRender_Init failed\n");
        GWindow_Destroy();
        GCore_Shutdown();
        return 1;
    }
    // 3D 游戏：走标准 3D 渲染管线（与编辑器 3D 模式一致）
    GViewport_SetSize(width, height);

    if (!scene_override.empty()) {
        if (GScene_Load(scene_override.c_str()) != 0) {
            std::fprintf(stderr, "[FPSDemo] failed to load scene %s\n",
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
                std::printf("[FPSDemo] auto-close after %.1f seconds\n",
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

    // 清理顺序：先销毁 Core 世界（释放 GPU 资源需要渲染上下文仍存活），
    // 再停物理后端，最后销毁渲染器与窗口（与 3dtest 的顺序一致，2dDemo 沿用了
    // 旧的错误顺序会在退出时挂死）。
    std::printf("[FPSDemo] shutdown core...\n");
    GCore_Shutdown();
    std::printf("[FPSDemo] shutdown physics...\n");
    GPhysics_Shutdown();
    std::printf("[FPSDemo] shutdown renderer...\n");
    GRender_Shutdown();
    std::printf("[FPSDemo] destroy window...\n");
    GWindow_Destroy();
    std::printf("[FPSDemo] exited\n");
    return 0;
}
