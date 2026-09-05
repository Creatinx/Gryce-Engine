#include "GryceEngineUtils/ui/file_watcher.h"

#include <algorithm>
#include <filesystem>

#include "utils/glog/glog_lib.h"

namespace GryceEngineUtils::ui {

// ============================================================================
// 辅助方法
// ============================================================================

std::filesystem::file_time_type FileWatcher::get_last_write_time(const std::string& path) {
    std::error_code ec;
    auto ft = std::filesystem::last_write_time(path, ec);
    if (ec) {
        // 文件不存在或无法访问，返回最小时间点
        return std::filesystem::file_time_type::min();
    }
    return ft;
}

// ============================================================================
// 注册 / 取消监听
// ============================================================================

void FileWatcher::watch(const std::string& path, FileChangeCallback callback,
                         int interval_ms) {
    if (path.empty() || !callback) return;

    // 检查是否已监听该路径
    auto it = std::find_if(entries_.begin(), entries_.end(),
        [&path](const Entry& e) { return e.path == path; });
    if (it != entries_.end()) {
        // 更新回调
        it->callback = std::move(callback);
        it->interval_ms = interval_ms;
        return;
    }

    Entry entry;
    entry.path = path;
    entry.callback = std::move(callback);
    entry.interval_ms = interval_ms;
    entry.last_write_time = get_last_write_time(path);
    {
        std::error_code ec;
        entry.last_size = std::filesystem::file_size(path, ec);
    }
    entry.has_initial_time = true;

    entries_.push_back(std::move(entry));
    GLOG_DEBUG("FileWatcher: watching '{}'", path);
}

void FileWatcher::unwatch(const std::string& path) {
    entries_.erase(
        std::remove_if(entries_.begin(), entries_.end(),
            [&path](const Entry& e) { return e.path == path; }),
        entries_.end());
}

void FileWatcher::clear() {
    entries_.clear();
}

// ============================================================================
// 轮询检测
// ============================================================================

int FileWatcher::poll() {
    int changed = 0;

    for (auto it = entries_.begin(); it != entries_.end(); ) {
        Entry& entry = *it;

        // 检查文件是否存在
        std::error_code ec;
        bool exists = std::filesystem::exists(entry.path, ec);
        if (!exists || ec) {
            ++it;
            continue;
        }

        // 获取当前修改时间和文件大小
        auto current_time = get_last_write_time(entry.path);
        std::error_code size_ec;
        uintmax_t current_size = std::filesystem::file_size(entry.path, size_ec);
        if (current_time == std::filesystem::file_time_type::min()) {
            ++it;
            continue;
        }

        // 检测变化：比较时间戳或文件大小
        bool changed_now = false;
        if (entry.has_initial_time) {
            if (current_time != entry.last_write_time ||
                current_size != entry.last_size) {
                changed_now = true;
            }
        } else {
            // 首次初始化
            entry.last_write_time = current_time;
            entry.last_size = current_size;
            entry.has_initial_time = true;
        }

        if (changed_now) {
            // 文件已修改，触发回调
            GLOG_INFO("FileWatcher: file changed '{}'", entry.path);
            if (entry.callback) {
                entry.callback(entry.path);
            }
            changed++;
            entry.last_write_time = current_time;
            entry.last_size = current_size;

            if (it == entries_.end()) break;
        }

        ++it;
    }

    return changed;
}

} // namespace GryceEngineUtils::ui