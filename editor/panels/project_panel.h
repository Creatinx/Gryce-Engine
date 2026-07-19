#pragma once

#include "../editor_panel.h"

#include <functional>
#include <filesystem>
#include <string>

namespace gryce_engine::editor {

// ---------------------------------------------------------------------------
// ProjectPanel — 项目资源浏览器（M1-E3）
//
// 显示项目根目录下的文件/文件夹，支持：
//   - 点击文件夹进入，顶部返回上一级
//   - 双击文件：由 EditorApp 根据扩展名决定打开场景 / 实例化模型等
//   - 拖拽任意文件到视口 / Hierarchy / Inspector（payload: GRYCE_PROJECT_FILE）
//
// 回调由 EditorApp 注入，保持面板本身不依赖具体场景操作。
// ---------------------------------------------------------------------------
class ProjectPanel : public EditorPanel {
public:
    ProjectPanel();

    // 文件被双击时调用（参数为 res:/ 相对路径）
    std::function<void(const std::string&)> on_activate_file;

protected:
    void on_imgui() override;

private:
    std::string to_res_path(const std::filesystem::path& absolute) const;
    void navigate_to(const std::filesystem::path& path);
    void draw_path_bar();
    void draw_entry(const std::filesystem::directory_entry& entry);

    std::filesystem::path current_dir_;
};

} // namespace gryce_engine::editor
