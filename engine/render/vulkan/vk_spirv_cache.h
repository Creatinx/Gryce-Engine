#pragma once

#include "vk_glsl_compiler.h"

#include <cstdint>
#include <string>
#include <vector>

namespace gryce_engine::render {

// 本次 SPIR-V 的来源，用于日志与统计。
enum class SpirvCacheSource {
    None,   // 未命中任何缓存，本次由 shaderc 实际编译
    Memory, // 命中进程内缓存
    Disk,   // 命中磁盘持久化缓存
};

// GLSL → SPIR-V 的统一获取入口，顺序为：内存缓存 → 磁盘缓存 → shaderc 编译并写盘。
// 3D 管线（vk_shader）与 2D 渲染器（vk_renderer2d）共用同一套缓存，
// 避免任何一条路径每次启动都重新编译。
//
//   source     完整 GLSL 源码，内存缓存键与磁盘缓存指纹均基于它
//   file_name  源码文件名，仅用于 shaderc 报错信息
//   spv_path   磁盘缓存路径（.spv）；同目录生成 .hash 边车记录源码指纹。
//              传空字符串表示只使用内存缓存、不落盘。
//   stage      着色阶段
//   out        成功时填充 SPIR-V word 序列
//   error      失败时写入可读错误
//   from       可选输出，标记本次结果来源
//
// 返回 true 表示 out 已填充。命中磁盘缓存时不会触发 shaderc 加载/编译。
bool load_or_compile_spirv(const std::string& source,
                           const std::string& file_name,
                           const std::string& spv_path,
                           GlslStage stage,
                           std::vector<uint32_t>& out,
                           std::string& error,
                           SpirvCacheSource* from = nullptr);

} // namespace gryce_engine::render
