#pragma once

namespace gryce_engine::editor {

// ---------------------------------------------------------------------------
// EditorApp — 编辑器应用入口（M1-E1）
// 承载编辑器主循环：ImGui Docking 布局 + 面板框架、自由飞行相机、
// GLOG Console 面板、渲染到纹理的 Viewport 面板。
// main.cpp 仅负责调用 EditorApp::run()。
// ---------------------------------------------------------------------------
class EditorApp {
public:
    EditorApp() = default;
    ~EditorApp() = default;

    EditorApp(const EditorApp&) = delete;
    EditorApp& operator=(const EditorApp&) = delete;

    // 返回进程退出码
    int run(int argc, char* argv[]);
};

} // namespace gryce_engine::editor
