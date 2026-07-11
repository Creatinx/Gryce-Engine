#include "project.h"

#include <algorithm>

namespace gryce_engine::resources {

Project& Project::instance() {
    static Project project;
    return project;
}

void Project::set_root(const std::string& root) {
    root_ = root;
    // 统一去掉末尾斜杠，方便后续拼接
    if (!root_.empty()) {
        char last = root_.back();
        if (last == '/' || last == '\\') {
            root_.pop_back();
        }
    }
}

const std::string& Project::root() const {
    return root_;
}

} // namespace gryce_engine::resources
