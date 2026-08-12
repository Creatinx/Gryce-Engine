// GryceEngine game entry (GryceSPC template).
// A minimal standalone executable produced by GryceGC: it links the core
// libraries and drives the whole game loop through the C API only:
//   Core init -> physics attach -> Platform window -> Renderer -> play loop.
#include "GryceCore/core_api.h"
#include "GryceCore/scene_api.h"
#include "GryceCore/script_api.h"
#include "GrycePlatform/window_api.h"
#include "GryceRenderer/render_api.h"
#include "GrycePhysics/physics_api.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

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
            for (const wchar_t* name : kCrtNames) {
                const std::filesystem::path sys_crt = std::filesystem::path(sys_dir) / name;
                LoadLibraryExW(sys_crt.c_str(), nullptr, 0);  // ignore: system may lack it
            }
        }

        // Engine DLLs and (as a fallback) the CRT resolve from runtime/.
        SetDllDirectoryW(runtime_dir.c_str());
    }
#endif

    argv0_override = argv[0];
    std::string project = default_project_root();
    const char* scene = "res:/scenes/main.gesc";
    bool scene_override = false;
    int width = 1280;
    int height = 720;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--project") == 0 && i + 1 < argc) project = argv[++i];
        else if (std::strcmp(argv[i], "--scene") == 0 && i + 1 < argc) { scene = argv[++i]; scene_override = true; }
        else if (std::strcmp(argv[i], "--w") == 0 && i + 1 < argc) width = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--h") == 0 && i + 1 < argc) height = std::atoi(argv[++i]);
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

    if (scene_override && GScene_Load(scene) != 0) {
        std::fprintf(stderr, "[game] failed to load scene %s\n", scene);
    }
    enter_play_mode();

    auto last = std::chrono::steady_clock::now();
    while (!GWindow_ShouldClose()) {
        GWindow_PollEvents();
        const auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        if (dt < 0.0f || dt > 0.05f) dt = 0.016f;

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
