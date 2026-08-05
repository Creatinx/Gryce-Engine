#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace gryce_engine::editor {

// ---------------------------------------------------------------------------
// FileBrowserPopup — 可复用的资源文件浏览弹窗
//
// 用法：在 res:/ 路径输入框旁调用 browse_button()，绘制「浏览...」按钮；
// 点击后打开模态文件浏览器（以项目根目录 res:/ 为根，详细列表视图），
// 确认后把选中的 res:/ 路径写回目标缓冲区。
//
// 单例实现：同一时刻只有一个浏览弹窗处于打开状态；
// 弹窗由发起它的那个输入框所在窗口负责绘制。
// ---------------------------------------------------------------------------
class FileBrowserPopup {
public:
    static FileBrowserPopup& instance();

    // 在路径输入框旁调用（通常接在 SameLine() 之后）。
    // 本帧用户确认选择时返回 true，且 buf 已被更新为 res:/ 相对路径。
    bool browse_button(const char* id, char* buf, size_t buf_size);

    // 「浏览...」按钮的占位宽度（文本 + FramePadding + 安全余量）。
    // 调用方用它为按钮预留空间（如固定列宽），避免面板过窄时按钮被挤出。
    static float browse_button_width();

private:
    FileBrowserPopup() = default;

    struct Entry {
        std::string name;     // UTF-8 显示名
        std::filesystem::path path;
        bool is_dir = false;
        std::uintmax_t size = 0;
        std::string type;     // 资源类型（文件夹 / texture / mesh ...）
        std::string modified; // 已格式化的修改时间
    };

    void open_for(char* buf, size_t buf_size);
    void refresh_entries();
    bool draw_popup(); // 返回 true 表示本帧确认了选择
    std::string current_res_dir() const;
    void confirm_selection();

    char* target_buf_ = nullptr;
    size_t target_size_ = 0;
    bool open_requested_ = false;

    std::string root_str_; // Project 根目录原始字符串（make_relative 用）
    std::filesystem::path root_;
    std::filesystem::path current_dir_;
    std::vector<Entry> entries_;
    int selected_ = -1;
};

} // namespace gryce_engine::editor
