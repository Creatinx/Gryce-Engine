#include "resource_path.h"

#include "project.h"

#include <algorithm>
#include <cctype>

namespace gryce_engine::resources {

bool ResourcePath::is_resource_path(const std::string& path) {
    if (path.size() < 5) return false;
    return (path[0] == 'r' || path[0] == 'R') &&
           (path[1] == 'e' || path[1] == 'E') &&
           (path[2] == 's' || path[2] == 'S') &&
           path[3] == ':' &&
           path[4] == '/';
}

std::string ResourcePath::resolve(const std::string& path, const std::string& project_root) {
    if (!is_resource_path(path)) {
        return path;
    }
    std::string relative = path.substr(5); // 去掉 "res:/"
    std::string root = project_root;
    if (!root.empty()) {
        char last = root.back();
        if (last == '/' || last == '\\') {
            root.pop_back();
        }
    }
    // res:/path 解析为 {project_root}/path
    // res:/ 本身就是游戏项目的虚拟根目录，不再额外拼接 /res/
    return root + "/" + relative;
}

std::string ResourcePath::resolve(const std::string& path) {
    return resolve(path, Project::instance().root());
}

std::string ResourcePath::make_relative(const std::string& absolute_path, const std::string& project_root) {
    std::string abs = absolute_path;
    std::string root = project_root;

    // 统一为正斜杠
    auto to_slash = [](std::string& s) {
        for (char& c : s) {
            if (c == '\\') c = '/';
        }
    };
    to_slash(abs);
    to_slash(root);

    if (!root.empty() && root.back() == '/') {
        root.pop_back();
    }

    if (abs.size() >= root.size() && abs.compare(0, root.size(), root) == 0) {
        std::string rel = abs.substr(root.size());
        if (!rel.empty() && rel.front() == '/') {
            rel = rel.substr(1);
        }
        return "res:/" + rel;
    }
    return absolute_path;
}

} // namespace gryce_engine::resources
