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
    argv0_override = argv[0];
    std::string project = default_project_root();
    const char* scene = "res:/scenes/main.gesc";
    int width = 1280;
    int height = 720;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--project") == 0 && i + 1 < argc) project = argv[++i];
        else if (std::strcmp(argv[i], "--scene") == 0 && i + 1 < argc) scene = argv[++i];
        else if (std::strcmp(argv[i], "--w") == 0 && i + 1 < argc) width = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--h") == 0 && i + 1 < argc) height = std::atoi(argv[++i]);
    }

    GCoreInitDesc core_desc{};
    core_desc.version = sizeof(GCoreInitDesc);
    core_desc.project_root = project.c_str();
    core_desc.enable_reflection = true;
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

    if (GScene_Load(scene) != 0) {
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
