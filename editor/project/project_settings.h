#pragma once

#include <string>

#include "render/render.h"

namespace gryce_engine::editor {

// ---------------------------------------------------------------------------
// ProjectSettings — 项目级设置（保存到 project_settings.json）
// ---------------------------------------------------------------------------
// 这些设置通常需要在编辑器启动前读取（例如渲染后端），因此与运行时
// EditorSettings 分开存储。当前支持：
//   - render_api: 默认渲染后端（OpenGL / Vulkan），命令行 --vulkan 可覆盖。
// ---------------------------------------------------------------------------

struct ProjectSettings {
    render::RenderAPI render_api = render::RenderAPI::OpenGL;
};

// 从项目根目录加载 project_settings.json；文件不存在时返回默认设置。
ProjectSettings load_project_settings(const std::string& project_root);

// 保存项目设置到项目根目录。
void save_project_settings(const std::string& project_root, const ProjectSettings& settings);

// 渲染后端字符串转换。
std::string render_api_to_string(render::RenderAPI api);
render::RenderAPI render_api_from_string(const std::string& s);

} // namespace gryce_engine::editor
