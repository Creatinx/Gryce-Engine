#pragma once

#include <string>

namespace gryce_engine::render::core_shaders {

// Core 默认 shader 集的虚拟资源根。渲染器在打包产物里也以此为 bundle 键，
// 使"项目覆盖 → core 兜底"两级解析能命中同一套命名空间。
constexpr const char* kCoreShadersResRoot = "res:/shaders";

// 返回磁盘上引擎默认 shader 目录（未打包/编辑器模式定位源码目录或随包部署
// 副本），找不到返回空串。解析器在项目磁盘文件、已挂载 bundle 之后以它兜底。
// 目录内容须包含根级 *_vert/*_frag 与 forward_clustered/ 子目录。
std::string engine_shaders_dir();

} // namespace gryce_engine::render::core_shaders