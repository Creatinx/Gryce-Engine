#pragma once

#include <filesystem>
#include <string>

namespace gryce_engine::editor {

// ---------------------------------------------------------------------------
// FileExplorerPanel — Godot 风格项目资源浏览器
// 根目录固定为项目根（显示为 res://），目录/文件用图标 + 名字展示，
// 双击目录进入，无法越出 res:// 根。
// ---------------------------------------------------------------------------
class FileExplorerPanel {
public:
    void set_root(const std::filesystem::path& root);
    void render();
    void render_as_tab(bool file_system_tab = true);

private:
    enum class EntryKind {
        Directory,
        Image,
        Audio,
        Model3D,
        Scene,
        Material,
        Script,
        Text,
        File,
    };

    struct EntryStyle {
        EntryKind kind = EntryKind::File;
        float r = 0.45f;
        float g = 0.48f;
        float b = 0.52f;
    };

    EntryStyle classify(const std::filesystem::path& path) const;
    void navigate_to(const std::filesystem::path& path);
    void navigate_up();
    bool is_hidden(const std::filesystem::path& path) const;
    std::string display_path() const;
    void render_breadcrumbs();
    void render_list();

    std::filesystem::path root_;
    std::filesystem::path current_;
    std::filesystem::path selected_;
};

} // namespace gryce_engine::editor
