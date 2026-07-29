#pragma once

#include "../editor_panel.h"

#include <functional>
#include <filesystem>
#include <string>
#include <vector>

namespace gryce_engine::editor {

class CommandStack;

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

    void set_undo_stack(CommandStack* stack) { undo_stack_ = stack; }

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

    // 右键菜单与新建/重命名/删除
    void draw_new_submenu();
    void draw_item_context_menu(const GridItem& item);
    void draw_popups();
    std::filesystem::path unique_path(const std::string& base, const std::string& ext) const;
    void create_folder();
    void create_scene();
    void create_material();
    void start_rename(const std::filesystem::path& target);
    void request_delete(const std::filesystem::path& target);

    // 复制 / 剪切 / 粘贴（内部剪贴板）与属性弹窗
    void clipboard_copy(const std::filesystem::path& target, bool is_cut);
    void paste_into_current_dir();
    void show_properties(const std::filesystem::path& target);
    std::filesystem::path unique_destination(const std::filesystem::path& file_name) const;

    std::filesystem::path current_dir_;

    CommandStack* undo_stack_ = nullptr;

    // 文件级剪贴板：粘贴时按 is_cut 决定移动还是复制
    std::filesystem::path clipboard_path_;
    bool clipboard_is_cut_ = false;

    bool properties_popup_requested_ = false;
    std::filesystem::path properties_target_;

    bool rename_popup_requested_ = false;
    char rename_buf_[256] = {};
    std::filesystem::path rename_target_;
    bool delete_popup_requested_ = false;
    std::filesystem::path delete_target_;
};

} // namespace gryce_engine::editor
