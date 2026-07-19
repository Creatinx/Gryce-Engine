// Gryce Engine 编辑器入口（M1-E1）
// 主循环与面板框架见 editor_app.cpp（EditorApp）。

#include "editor_app.h"

int main(int argc, char* argv[]) {
    return gryce_engine::editor::EditorApp().run(argc, argv);
}
