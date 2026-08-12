#pragma once

#include "export.h"

#include <string>

namespace gryce_engine::resources {

// ---------------------------------------------------------------------------
// Project — 项目根目录上下文
// 单例，保存当前项目根路径，用于把 res:/ 解析为绝对路径。
// ---------------------------------------------------------------------------
class GRYCE_API Project {
public:
    static Project& instance();

    void set_root(const std::string& root);
    const std::string& root() const;

    /// Main scene: the scene a game enters on startup (res:/ path).
    /// Defaults to "res:/scenes/main.gesc", overridable via the
    /// project_settings.json "main_scene" key.
    void set_main_scene(const std::string& path);
    const std::string& main_scene() const;

private:
    Project() = default;

    std::string root_;
    std::string main_scene_ = "res:/scenes/main.gesc";
};

} // namespace gryce_engine::resources
