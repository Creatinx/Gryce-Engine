#pragma once

#include "../editor_panel.h"

#include <functional>
#include <filesystem>
#include <string>
#include <vector>

namespace gryce_engine::editor {

// ---------------------------------------------------------------------------
// FileExplorerPanel — 文件资源管理器（原 ProjectPanel）
//
// 显示项目根目录下的文件/文件夹，支持：
//   - 点击文件夹进入，顶部返回上一级
//   - 双击文件：由 EditorApp 根据扩展名决定打开场景 / 实例化模型等
//   - 拖拽任意文件到视口 / Hierarchy / Inspector（payload: GRYCE_PROJECT_FILE）
//
// 回调由 EditorApp 注入，保持面板本身不依赖具体场景操作。
// ---------------------------------------------------------------------------
class FileExplorerPanel : public EditorPanel {
public:
    FileExplorerPanel();

    // 文件被双击时调用（参数为 res:/ 相对路径）
    std::function<void(const std::string&)> on_activate_file;

protected:
    void on_imgui() override;

private:
    struct GridItem {
        std::filesystem::directory_entry entry;
        std::string name;
        bool is_dir = false;
        std::vector<std::string> lines;
        float height = 0.0f;
    };

    std::string to_res_path(const std::filesystem::path& absolute) const;
    void navigate_to(const std::filesystem::path& path);
    void draw_path_bar();
    void draw_grid_item(const GridItem& item, ImVec2 pos, float item_width, float scale);

    std::filesystem::path current_dir_;
};

} // namespace gryce_engine::editor
