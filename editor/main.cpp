// Gryce Engine 编辑器入口（M1-E1）
// 主循环与面板框架见 editor_app.cpp（EditorApp）。

#ifdef _WIN32
#include <windows.h>
#endif

#include "editor_app.h"

int main(int argc, char* argv[]) {
#ifdef _WIN32
    // 控制台输出使用 UTF-8，避免中文日志乱码
    SetConsoleOutputCP(CP_UTF8);
#endif
    return gryce_engine::editor::EditorApp().run(argc, argv);
}
