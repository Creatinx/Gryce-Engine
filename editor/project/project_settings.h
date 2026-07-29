#pragma once

#include <string>

#include "render/render.h"

namespace gryce_engine::editor {

// ---------------------------------------------------------------------------
// ProjectSettings — 项目级设置（保存到 project_settings.json）
// ---------------------------------------------------------------------------
// 这些设置通常需要在编辑器启动前读取（例如渲染后端），因此与运行时
// EditorSettings 分开存储。当前支持：
//   - render_api: 默认渲染后端（Vulkan 默认 / OpenGL 兼容；DX11/12 预留）。
//     命令行 --vulkan / --opengl 可覆盖。
//   - quality: 阴影/HDR/环境光等渲染质量选项，重启后生效。
// ---------------------------------------------------------------------------

// 渲染质量选项（持久化到 project_settings.json 的 "graphics" 组）
struct RenderQualitySettings {
    int shadow_map_size = 2048;        // 阴影贴图分辨率（512/1024/2048/4096）
    float shadow_bias = 0.001f;        // 阴影深度偏移
    float shadow_area = 15.0f;         // 阴影正交盒半径（世界单位）
    float ambient[3] = {0.15f, 0.15f, 0.15f}; // 环境光颜色
    bool hdr_enabled = true;           // HDR + tone mapping
    float exposure = 2.0f;             // 曝光
    int tone_map_mode = 1;             // 0: none, 1: reinhard, 2: aces
    float ibl_intensity = 1.0f;        // IBL 环境光照强度
};

struct ProjectSettings {
    render::RenderAPI render_api = render::RenderAPI::Vulkan;
    RenderQualitySettings quality;
};

// 从项目根目录加载 project_settings.json；文件不存在时返回默认设置。
ProjectSettings load_project_settings(const std::string& project_root);

// 保存项目设置到项目根目录。
void save_project_settings(const std::string& project_root, const ProjectSettings& settings);

// 渲染后端字符串转换。
std::string render_api_to_string(render::RenderAPI api);
render::RenderAPI render_api_from_string(const std::string& s);

} // namespace gryce_engine::editor
