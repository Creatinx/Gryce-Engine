#pragma once

#include <string>

namespace gryce_engine::resources {

// ---------------------------------------------------------------------------
// ResourcePath — 资源路径处理
// res:/path/to/file 表示相对于项目根目录的路径。
// ---------------------------------------------------------------------------
class ResourcePath {
public:
    // 判断是否为 res:/ 开头的资源路径
    static bool is_resource_path(const std::string& path);

    // 将 res:/path 解析为 {project_root}/path
    static std::string resolve(const std::string& path, const std::string& project_root);

    // 使用 Project::instance().root() 解析
    static std::string resolve(const std::string& path);

    // 将绝对路径转换为 res:/ 相对路径（便于保存）
    static std::string make_relative(const std::string& absolute_path, const std::string& project_root);
};

} // namespace gryce_engine::resources
