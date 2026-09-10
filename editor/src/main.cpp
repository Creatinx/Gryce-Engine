// GryceEditor — 主入口
// 无 --project 参数时进入项目启动器（新建/打开项目）；
// 带 --project <路径> 时直接打开指定项目。

#include <cstring>

#include "editor.h"
#include "splash_process.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

int run_launcher();

int main(int argc, char* argv[]) {
#ifdef _WIN32
    // DPI-aware：让 GLFW 窗口/帧缓冲使用真实物理像素，避免 DWM 虚拟化缩放
    // 导致窗口与渲染内容尺寸不一致（与 FPSDemo/2dDemo 一致的做法）。
    {
        using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(void*);
        if (HMODULE user32 = ::GetModuleHandleW(L"user32.dll")) {
            auto fn = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
                ::GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
            if (fn) {
                fn(reinterpret_cast<void*>(-4)); // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
            }
        }
    }
#endif

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--splash") == 0) {
            // 启动画面子进程分支：显示加载动画，直到 marker 文件出现才退出。
            const char* marker = (i + 1 < argc) ? argv[++i] : nullptr;
            return gryce_engine::editor::run_splash_process(marker);
        }
        if (std::strcmp(argv[i], "--project") == 0) {
            gryce_engine::editor::EditorApp app;
            if (!app.init(argc, argv)) {
                return 1;
            }
            int result = app.run();
            app.shutdown();
            return result;
        }
    }

    // 未指定项目：先进启动器（项目管理器）
    return run_launcher();
}
