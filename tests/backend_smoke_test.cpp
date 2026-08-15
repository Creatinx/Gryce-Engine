#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "GryceCore/core_api.h"
#include "GrycePlatform/window_api.h"
#include "GryceRenderer/render_api.h"

// 渲染后端冒烟测试：走最小 C API 路径创建 GL 窗口 -> 初始化渲染器 ->
// 驱动一帧 -> 干净关闭。无显示环境（CI headless）时优雅跳过。
// 该测试验证 DLL 边界导出、GL 上下文归属与帧驱动主链路没有回归。
TEST(BackendSmokeTest, OpenGLInitAndRenderOneFrame) {
    namespace fs = std::filesystem;

    fs::path root = fs::temp_directory_path() / "gryce_smoke_project";
    std::error_code ec;
    fs::create_directories(root, ec);
    std::string root_str = root.string();

    auto cleanup = [&]() {
        if (GRender_IsInitialized()) GRender_Shutdown();
        if (GWindow_IsValid()) GWindow_Destroy();
        if (GCore_IsInitialized()) GCore_Shutdown();
        fs::remove_all(root, ec);
    };

    GCoreInitDesc core_desc{};
    core_desc.version = static_cast<uint32_t>(sizeof(core_desc));
    core_desc.project_root = root_str.c_str();
    core_desc.enable_reflection = true;
    if (GCore_Init(&core_desc) != 0) {
        GTEST_SKIP() << "GCore_Init failed (no display?)";
    }

    if (GWindow_Create("gryce-smoke", 320, 240, GWINDOW_MODE_WINDOWED) != 0) {
        cleanup();
        GTEST_SKIP() << "GWindow_Create failed (headless environment?)";
    }

    GRenderInitDesc render_desc{};
    render_desc.version = static_cast<uint32_t>(sizeof(render_desc));
    render_desc.native_window = GWindow_GetNativeHandle();
    render_desc.api = GRYCE_RENDER_API_OPENGL;
    render_desc.viewport_w = 320;
    render_desc.viewport_h = 240;
    render_desc.sync_mode = true;
    if (GRender_Init(&render_desc) != 0) {
        cleanup();
        GTEST_SKIP() << "GRender_Init failed (no GL context?)";
    }

    // 空世界驱动一帧：BeginFrame -> RenderWorld -> EndFrame
    GRender_BeginFrame();
    GRender_RenderWorld();
    GRender_EndFrame();

    EXPECT_TRUE(GRender_IsInitialized());
    cleanup();
}
