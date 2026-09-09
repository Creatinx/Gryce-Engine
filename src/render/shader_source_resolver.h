#pragma once

#include <string>

namespace gryce_engine::render {

// RenderAPI 在 render.h（OpenGL / Vulkan 等）
enum class RenderAPI;

// 一次 shader 源码解析的结果：既含源码文本（供运行时编译），也含实际命中的
// 可读文件路径（磁盘文件路径，或 bundle 解出的临时路径），供热重载记录 mtime。
struct ShaderSourceSet {
    std::string vertex;        // 顶点源码
    std::string fragment;      // 片段源码
    std::string vertex_path;   // 命中顶点源的可读路径
    std::string fragment_path; // 命中片段源的可读路径
    bool valid() const { return !vertex.empty() && !fragment.empty(); }
};

// 两级解析 + core 兜底，GL/Vulkan 共用：
//   1. 项目磁盘文件（res:/ 解析后的真实文件）
//   2. 已挂载 bundle（打包产物，含项目覆盖与随项目打包的 core 兜底）
//   3. 引擎默认 shader 目录（未打包/编辑器模式）
// 任一样取到即为命中；全空返回 invalid。name 为不带扩展名的 shader 基名，
// shader_dir 为目录（如 "res:/shaders" 或 "res:/shaders/forward_clustered"）。
ShaderSourceSet resolve_shader_source(const std::string& name,
                                      const std::string& shader_dir,
                                      RenderAPI api);

// 单阶段解析（顶点/片段各自独立）：用于只有一个阶段的 shader（如 2D bloom
// 各 pass 仅 .frag，顶点与 Downsample 共用）。ext 为 ".vert" / ".frag"。
struct ShaderStageSource {
    std::string code;      // 源码文本
    std::string path;      // 命中源的可读路径
    bool ok() const { return !code.empty() && !path.empty(); }
};
ShaderStageSource resolve_shader_stage_source(const std::string& name,
                                              const std::string& shader_dir,
                                              const char* ext,
                                              RenderAPI api);

} // namespace gryce_engine::render