#pragma once

#include <string>

#include <imgui.h>

#include "../project/project_settings.h"
#include "../localization/localization.h"

namespace gryce_engine::editor {

// ---------------------------------------------------------------------------
// ProjectSettingsWindow — 项目设置窗口（File > Project Settings）
// ---------------------------------------------------------------------------
// 当前栏目：
//   - Graphics：渲染后端等需要在重启后生效的选项。
// ---------------------------------------------------------------------------

class ProjectSettingsWindow {
public:
    // 尝试从项目根目录加载 project_settings.json；失败则返回默认设置。
    static ProjectSettings load(const std::string& project_root);

    // 保存当前设置到项目根目录。
    static void save(const std::string& project_root, const ProjectSettings& settings);

    // 绘制窗口。若窗口仍打开返回 true，关闭后返回 false。
    bool draw(const std::string& project_root, ProjectSettings& settings);

    void open() { open_ = true; }
    bool is_open() const { return open_; }

private:
    enum class Section { Graphics };

    void draw_sidebar(float width);
    void draw_graphics_section(ProjectSettings& settings);

    bool open_ = false;
    Section current_section_ = Section::Graphics;
    bool unsaved_changes_ = false;
    std::string project_root_;
};

} // namespace gryce_engine::editor
