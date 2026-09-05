#pragma once

// GryceEngineUtils/ui/file_watcher.h
//
// FileWatcher — 文件监听系统（轮询方式）
//
// 功能：
// 1. 注册待监听的文件路径
// 2. 以固定间隔轮询文件的 last_write_time
// 3. 检测到文件变化时触发回调函数
// 4. 支持 .uif 和 .js 文件的热重载通知
//
// 用法：
//   FileWatcher watcher;
//   watcher.watch("ui/main_menu.uif", [](const std::string& path) {
//       GLOG_INFO("File changed: {}", path);
//   });
//   watcher.poll();  // 在主循环中定期调用

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace GryceEngineUtils::ui {

// ---------------------------------------------------------------------------
// FileWatcher — 文件监听器
// ---------------------------------------------------------------------------
class FileWatcher {
public:
    using FileChangeCallback = std::function<void(const std::string& path)>;

    FileWatcher() = default;
    ~FileWatcher() = default;

    FileWatcher(const FileWatcher&) = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;

    // 注册一个文件监听
    // path: 文件的绝对路径
    // callback: 文件变化时调用的回调函数
    // interval_ms: 该文件的轮询间隔（毫秒），默认 500ms
    void watch(const std::string& path, FileChangeCallback callback,
               int interval_ms = 500);

    // 取消监听某个文件
    void unwatch(const std::string& path);

    // 取消所有监听
    void clear();

    // 轮询所有注册的文件，检测变化
    // 返回本次检测到变化的文件数量
    int poll();

    // 返回当前监听的文件数量
    size_t watched_count() const { return entries_.size(); }

    // 设置全局轮询间隔（默认 200ms）
    void set_global_interval_ms(int ms) { global_interval_ms_ = ms; }

private:
    struct Entry {
        std::string path;
        FileChangeCallback callback;
        int interval_ms;
        std::filesystem::file_time_type last_write_time;
        uintmax_t last_size = 0;
        bool has_initial_time = false;
    };

    std::vector<Entry> entries_;
    int global_interval_ms_ = 200;

    // 获取文件的最后修改时间（失败时返回默认时间点）
    static std::filesystem::file_time_type get_last_write_time(const std::string& path);
};

} // namespace GryceEngineUtils::ui