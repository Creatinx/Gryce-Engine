#pragma once

#include <string>

namespace gryce_engine::resources {

// ---------------------------------------------------------------------------
// Project — 项目根目录上下文
// 单例，保存当前项目根路径，用于把 res:/ 解析为绝对路径。
// ---------------------------------------------------------------------------
class Project {
public:
    static Project& instance();

    void set_root(const std::string& root);
    const std::string& root() const;

private:
    Project() = default;

    std::string root_;
};

} // namespace gryce_engine::resources
