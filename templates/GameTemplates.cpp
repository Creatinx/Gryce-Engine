// GryceEngine game entry (GryceSPC template).
// A minimal standalone executable produced by GryceGC: it links the core
// libraries and drives the whole game loop through the C API only:
//   Core init -> physics attach -> Platform window -> Renderer -> play loop.
#include "GryceCore/core_api.h"
#include "GryceCore/scene_api.h"
#include "GryceCore/script_api.h"
#include "GrycePlatform/window_api.h"
#include "GrycePlatform/input_api.h"
#include "GryceRenderer/render_api.h"
#include "GrycePhysics/physics_api.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#include <delayimp.h>
#endif

namespace {

#if defined(_WIN32)
// Appends a diagnostic line to <exe dir>/gryce_boot.log so a failing machine
// reports what went wrong instead of dying silently.
void write_boot_log(const std::wstring& exe_dir, const std::string& line) {
    std::ofstream log(std::filesystem::path(exe_dir) / "gryce_boot.log", std::ios::app);
    if (log) log << line << "\n";
}

// Delay-load failure hook: logs the missing DLL, tries <exe dir>/runtime as a
// last resort, then shows the reason before the process terminates.
extern "C" FARPROC WINAPI GryceDelayLoadHook(unsigned event, PDelayLoadInfo info) {
    if (event != dliFailLoadLib || !info || !info->szDll) return nullptr;

    wchar_t exe_buf[MAX_PATH + 1] = {};
    std::wstring exe_dir;
    if (GetModuleFileNameW(nullptr, exe_buf, MAX_PATH) > 0) {
        exe_dir = std::filesystem::path(exe_buf).parent_path().wstring();
    }
    const std::string msg = std::string("delay-load failed: ") + info->szDll;
    write_boot_log(exe_dir, msg);

    // Last resort: resolve from <exe dir>/runtime even if the search path
    // was not set up (e.g. the CRT preload failed on a bare machine).
    if (!exe_dir.empty()) {
        const int wide_len = MultiByteToWideChar(CP_ACP, 0, info->szDll, -1, nullptr, 0);
        std::wstring wide(static_cast<size_t>(wide_len > 0 ? wide_len : 1), L'\0');
        if (wide_len > 0) {
            MultiByteToWideChar(CP_ACP, 0, info->szDll, -1, wide.data(), wide_len);
            const std::wstring full = exe_dir + L"\\runtime\\" + wide;
            HMODULE h = LoadLibraryExW(full.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (h) return reinterpret_cast<FARPROC>(h);
        }
    }

    MessageBoxW(nullptr,
                (L"GryceGame failed to start:\n" +
                 std::wstring(L"missing runtime DLL: ") +
                 [&]() {
                     std::wstring w;
                     const int n = MultiByteToWideChar(CP_ACP, 0, info->szDll, -1, nullptr, 0);
                     if (n > 0) {
                         w.resize(n - 1);
                         MultiByteToWideChar(CP_ACP, 0, info->szDll, -1, w.data(), n);
                     }
                     return w;
                 }() + L"\nSee gryce_boot.log next to the game.").c_str(),
                L"GryceGame", MB_OK | MB_ICONERROR);
    return nullptr;
}

PfnDliHook __pfnDliFailureHook2 = GryceDelayLoadHook;
#endif

// Set from argv[0] in main(); used as fallback when the platform API cannot
// resolve the executable path.
std::string argv0_override;

// Default project root: the directory of the executable. With GryceGC output
// the .gpkg archives live next to the .exe, so res:/ resolves from there even
// when the game is launched by double-click (CWD may be anywhere).
std::string default_project_root() {
#if defined(_WIN32)
    wchar_t buf[MAX_PATH + 1] = {};
    const DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        std::filesystem::path p(buf);
        return p.parent_path().string();
    }
#endif
    std::filesystem::path p(argv0_override.empty() ? "." : argv0_override);
    return std::filesystem::absolute(p).parent_path().string();
}

// 2D 项目在 project_settings.json 里声明 "scene_2d": true，
// 让渲染器走纯 2D 画布路径（跳过 3D 管线，与编辑器 2D 模式一致）。
bool project_is_2d(const std::string& root) {
    try {
        std::ifstream in(root + "/project_settings.json");
        if (!in) return false;
        nlohmann::json j;
        in >> j;
        return j.value("scene_2d", false);
    } catch (const std::exception&) {
        return false;
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
#if defined(_WIN32)
    // GryceGC output layout puts the runtime DLLs in the "runtime" subfolder
    // next to the exe; the core DLLs are delay-loaded so the search path can
    // be extended here before the first engine call.
    wchar_t exe_buf[MAX_PATH + 1] = {};
    const DWORD exe_len = GetModuleFileNameW(nullptr, exe_buf, MAX_PATH);
    if (exe_len > 0 && exe_len < MAX_PATH) {
        const std::filesystem::path exe_dir = std::filesystem::path(exe_buf).parent_path();
        const std::filesystem::path runtime_dir = exe_dir / "runtime";
        write_boot_log(exe_dir.wstring(), "gryce_boot: exe_dir=" + exe_dir.string());
        write_boot_log(exe_dir.wstring(),
                       "gryce_boot: runtime dir exists=" +
                       std::to_string(std::filesystem::is_directory(runtime_dir)));

        // Prefer the SYSTEM VC++ runtime: pin it by loading it from System32
        // explicitly. Once loaded, the delay-loaded engine DLLs bind to the
        // system version instead of the bundled copy in runtime/ (an
        // already-loaded module wins over the search path). If the system
        // lacks the runtime these loads just fail, and the engine DLLs then
        // resolve their CRT from runtime/ (the fallback below).
        wchar_t sys_dir[MAX_PATH + 1] = {};
        const UINT sys_len = GetSystemDirectoryW(sys_dir, MAX_PATH);
        if (sys_len > 0 && sys_len < MAX_PATH) {
            static const wchar_t* kCrtNames[] = {
                L"vcruntime140.dll", L"vcruntime140_1.dll", L"vcruntime140_threads.dll",
                L"msvcp140.dll", L"msvcp140_1.dll", L"msvcp140_2.dll",
                L"concrt140.dll", L"vccorlib140.dll", L"vcomp140.dll",
                L"vcruntime140d.dll", L"vcruntime140_1d.dll", L"vcruntime140_threadsd.dll",
                L"msvcp140d.dll", L"msvcp140_1d.dll", L"msvcp140_2d.dll",
                L"concrt140d.dll", L"vccorlib140d.dll", L"vcomp140d.dll",
            };
            int crt_loaded = 0;
            for (const wchar_t* name : kCrtNames) {
                const std::filesystem::path sys_crt = std::filesystem::path(sys_dir) / name;
                if (LoadLibraryExW(sys_crt.c_str(), nullptr, 0)) ++crt_loaded;  // ignore failures
            }
            write_boot_log(exe_dir.wstring(),
                           "gryce_boot: system CRT DLLs loaded=" + std::to_string(crt_loaded));
        }

        // Engine DLLs and (as a fallback) the CRT resolve from runtime/.
        SetDllDirectoryW(runtime_dir.c_str());
    }
#endif

    argv0_override = argv[0];
    std::string project = default_project_root();
    const char* scene = "res:/scenes/main.gesc";
    bool scene_override = false;
    float auto_close_seconds = 0.0f;
    int width = 1280;
    int height = 720;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--project") == 0 && i + 1 < argc) project = argv[++i];
        else if (std::strcmp(argv[i], "--scene") == 0 && i + 1 < argc) { scene = argv[++i]; scene_override = true; }
        else if (std::strcmp(argv[i], "--auto-close") == 0 && i + 1 < argc) {
            auto_close_seconds = static_cast<float>(std::atof(argv[++i]));
        }
        else if (std::strcmp(argv[i], "--w") == 0 && i + 1 < argc) width = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--h") == 0 && i + 1 < argc) height = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::printf(
                "GryceGame (GryceSPC 游戏模板)\n"
                "  --project <dir>   项目根（默认 exe 目录）\n"
                "  --scene <res:...> 覆盖起始场景\n"
                "  --auto-close <s>  运行 s 秒后自动退出（CI/测试用）\n"
                "  --w/--h <px>      窗口尺寸\n");
            return 0;
        }
    }

    GCoreInitDesc core_desc{};
    core_desc.version = sizeof(GCoreInitDesc);
    core_desc.project_root = project.c_str();
    core_desc.enable_reflection = true;
    // The core enters the project's main scene (project_settings.json
    // "main_scene", default res:/scenes/main.gesc) right after startup unless
    // an explicit --scene override is given.
    GCore_SetAutoLoadMainScene(!scene_override);
    if (GCore_Init(&core_desc) != 0) {
        std::fprintf(stderr, "[game] GCore_Init failed\n");
        return 1;
    }

    if (void* world = GCore_GetInternalWorldPtr()) {
        if (GPhysics_Init(GPHYSICS_BACKEND_JOLT) == 0) {
            GPhysics_AttachSystems(world);
        }
    }

    if (GWindow_Create("Gryce Game", width, height, GWINDOW_MODE_WINDOWED) != 0) {
        std::fprintf(stderr, "[game] GWindow_Create failed\n");
        GCore_Shutdown();
        return 1;
    }

    GRenderInitDesc render_desc{};
    render_desc.version = sizeof(GRenderInitDesc);
    render_desc.native_window = GWindow_GetRenderHandle();
    render_desc.api = GRYCE_RENDER_API_OPENGL;
    render_desc.viewport_w = width;
    render_desc.viewport_h = height;
    render_desc.sync_mode = true;
    if (GRender_Init(&render_desc) != 0) {
        std::fprintf(stderr, "[game] GRender_Init failed\n");
        GWindow_Destroy();
        GCore_Shutdown();
        return 1;
    }
    GRender_SetScene2D(project_is_2d(project));

    if (scene_override && GScene_Load(scene) != 0) {
        std::fprintf(stderr, "[game] failed to load scene %s\n", scene);
    }
    enter_play_mode();

    auto last = std::chrono::steady_clock::now();
    float auto_close_timer = 0.0f;
    while (!GWindow_ShouldClose()) {
        GWindow_PollEvents();
        GInput_SyncToCore();
        const auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        if (dt < 0.0f || dt > 0.05f) dt = 0.016f;

        if (auto_close_seconds > 0.0f) {
            auto_close_timer += dt;
            if (auto_close_timer >= auto_close_seconds) {
                std::printf("[game] auto-close after %.1f seconds\n", auto_close_seconds);
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
    return 0;
}
