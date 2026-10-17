// vk_glsl_compiler.h — GLSL → SPIR-V 编译入口（shaderc 运行时动态加载）：
// 声明着色阶段枚举、可用性探测与编译函数；shaderc 缺失时调用方回退到
// 预编译的 .spv 产物。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gryce_engine::render {

// GLSL 着色阶段，映射到 shaderc_vertex_shader / shaderc_fragment_shader /
// shaderc_compute_shader。
enum class GlslStage {
    Vertex,
    Fragment,
    Compute,
};

// 是否可在当前进程加载 shaderc（懒加载，仅首次调用 compile 时才校验）。
bool shaderc_available();

// 将 GLSL 源码编译为 SPIR-V word 序列。成功置 out 并返回 true；失败返回
// false 并在 error 中写入可读错误（编译报错或 shaderc 不可用）。
//
// shaderc 采用运行时动态加载（Windows LoadLibrary / POSIX dlopen）：
//   - 无链接期依赖 → GL-only 运行时不需要 shaderc DLL；
//   - 跨 MSVC / MinGW / Linux 一致，规避 shaderc 静态库的 .drectve/MSVC 兼容问题；
//   - shaderc 缺失或加载失败时不阻断，调用方回退 `.spv` 预编译产物。
bool compile_glsl_to_spirv(const std::string& source,
                           const std::string& file_name,
                           GlslStage stage,
                           std::vector<uint32_t>& out,
                           std::string& error);

} // namespace gryce_engine::render