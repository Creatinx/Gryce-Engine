// 启动器与 WebView2 桥共用的共享工具。
// 从 launcher.cpp 的匿名命名空间中抽取，避免桥接侧重复实现。

#ifndef GRYCE_EDITOR_LAUNCHER_SHARED_H
#define GRYCE_EDITOR_LAUNCHER_SHARED_H

#include <filesystem>
#include <string>
#include <vector>

namespace gryce_engine::editor::launcher {

// 当前可执行文件所在目录
std::filesystem::path executable_dir();

// launcher.json 配置文件路径（%APPDATA%/GryceEngine/launcher.json）
std::filesystem::path config_path();

// 最近项目路径列表（纯读/写文件，不缓存）
std::vector<std::string> load_recent();
void save_recent(const std::vector<std::string>& recent);
void push_recent(std::vector<std::string>& recent, const std::string& path);

// 启动器设置（持久化在 launcher.json 的 "settings" 字段）
struct LauncherSettings {
    std::string defaultDir;   // 默认新建项目目录
    std::string editorArgs;   // 额外传给 GryceEditor.exe 的启动参数
    std::string lastVersion;  // 上次选择的游戏版本
};
LauncherSettings load_settings();
void save_settings(const LauncherSettings& settings);

// 文件夹选择对话框（Windows IFileDialog）；非 Windows 返回空串
std::string pick_folder();

// 文件夹最后修改时间 -> "YYYY-MM-DD HH:MM"
std::string format_file_time(const std::filesystem::path& p);

// 拉起 GryceEditor.exe --project <path> [extraArgs]。成功返回 true。
bool launch_editor(const std::string& project_path, std::string& error,
                   const std::string& extra_args = "");

// 校验并创建项目目录（{parent}/{name}/scenes）。成功返回 true 并写出 out_dir。
bool create_project_dir(const std::string& name, const std::string& parent,
                        std::string& error, std::filesystem::path& out_dir);

} // namespace gryce_engine::editor::launcher

#endif // GRYCE_EDITOR_LAUNCHER_SHARED_H