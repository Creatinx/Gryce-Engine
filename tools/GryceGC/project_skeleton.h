// 创建标准 GryceGC-A 项目骨架。
#pragma once

#include <filesystem>
#include <string>

namespace gryce_engine::gc {

// 在 dir 创建标准 GryceGC-A 项目（scenes/scripts/shaders/... 子目录 + 唯一配置文件
// project.data + 最小空主场景 scenes/main.gesc）。目录已存在且非空时拒绝覆盖。
bool create_project_skeleton(const std::filesystem::path& dir,
                             const std::string& name,
                             const std::string& window_title);

} // namespace gryce_engine::gc