#pragma once

#include <string>
#include <memory>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <iostream>
#include <mutex>
#include <vector>
#include <format>
#include <source_location>

#if defined(_WIN32)
    #include <windows.h>
#endif

namespace gryce_engine::utils {

// ---------------------------------------------------------------------------
// LogLevel
// ---------------------------------------------------------------------------
enum class LogLevel {
    Trace = 0,
    Debug = 1,
    Info  = 2,
    Warn  = 3,
    Error = 4,
    Fatal = 5
};

// ---------------------------------------------------------------------------
// ILogger — 纯虚接口，所有日志后端必须继承
// ---------------------------------------------------------------------------
class ILogger {
public:
    virtual ~ILogger() = default;

    // 核心输出接口
    virtual void log(LogLevel level, const std::string& message) = 0;

    // 带源码位置的重载（默认实现忽略位置，仅转发到无位置版本）
    virtual void log(LogLevel level, const std::string& message, std::source_location loc) {
        (void)loc;
        log(level, message);
    }

    // 强制刷新缓冲区
    virtual void flush() {}

    // 是否启用 ANSI 颜色（供子类查询）
    virtual bool supports_color() const { return false; }
};

// ---------------------------------------------------------------------------
// ConsoleLogger — 默认后端，输出到 stderr，带 ANSI 颜色
// ---------------------------------------------------------------------------
class ConsoleLogger : public ILogger {
public:
    ConsoleLogger();
    void log(LogLevel level, const std::string& message) override;
    void log(LogLevel level, const std::string& message, std::source_location loc) override;
    void flush() override;
    bool supports_color() const override { return ansi_enabled_; }

private:
    bool ansi_enabled_ = false;
    const char* ansi_color(LogLevel level) const;
    static const char* level_str(LogLevel level);
    static std::string timestamp();
};

// ---------------------------------------------------------------------------
// LogEntry — 内存日志条目（供编辑器 Console 面板读取）
// ---------------------------------------------------------------------------
struct LogEntry {
    LogLevel level = LogLevel::Info;
    std::string message;
    std::string timestamp;
    std::string source_file;
    int source_line = 0;
};

// ---------------------------------------------------------------------------
// MemoryLogSink — 环形缓冲内存 sink（tee 模式，M1-E1 编辑器 Console 面板）
// 包装现有后端：日志照常转发到原后端，同时写入固定容量环形缓冲。
// 线程安全（独立 mutex），snapshot() 拷贝读取，渲染/逻辑线程均可安全写日志。
// ---------------------------------------------------------------------------
class MemoryLogSink : public ILogger {
public:
    explicit MemoryLogSink(std::unique_ptr<ILogger> inner, size_t capacity = 1000);

    void log(LogLevel level, const std::string& message) override;
    void log(LogLevel level, const std::string& message, std::source_location loc) override;
    void flush() override;
    bool supports_color() const override;

    // 拷贝一份当前缓冲内容（按时间顺序，最旧在前）
    std::vector<LogEntry> snapshot() const;
    void clear();
    size_t size() const;
    size_t capacity() const { return buffer_.size(); }

    // 从 GLog 当前后端取得 sink（未安装时返回 nullptr）
    static MemoryLogSink* from_glog();

    static const char* level_str(LogLevel level);

private:
    static std::string make_timestamp();

    std::unique_ptr<ILogger> inner_;
    std::vector<LogEntry> buffer_;  // 环形缓冲，容量固定
    size_t head_ = 0;               // 下一个写入位置
    size_t count_ = 0;              // 当前条目数（<= capacity）
    mutable std::mutex mutex_;
};

// ---------------------------------------------------------------------------
// GLog — 全局日志管理器（单例），公开 API 入口
// ---------------------------------------------------------------------------
class GLog {
public:
    // 获取单例
    static GLog& instance();

    // 更换日志后端（默认 ConsoleLogger）
    void set_logger(std::unique_ptr<ILogger> logger);

    // 获取当前后端
    ILogger* logger() const;

    // 设置/获取最小日志级别，低于此级别的日志不会输出（默认 Info）
    void set_min_level(LogLevel level);
    LogLevel min_level() const;

    // 强制 flush
    void flush();

    // -----------------------------------------------------------------------
    // 模板化日志接口（支持 std::format 风格）
    // -----------------------------------------------------------------------
    template<typename... Args>
    void log(LogLevel level, std::source_location loc, std::format_string<Args...> fmt, Args&&... args) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!logger_ || static_cast<int>(level) < static_cast<int>(min_level_)) return;
        std::string msg = std::format(fmt, std::forward<Args>(args)...);
        logger_->log(level, msg, loc);
    }

    // 静态便捷方法（直接调用单例）——无源码位置版本保留兼容性
    template<typename... Args>
    static void trace(std::format_string<Args...> fmt, Args&&... args) {
        instance().log(LogLevel::Trace, std::source_location{}, fmt, std::forward<Args>(args)...);
    }
    template<typename... Args>
    static void trace(std::source_location loc, std::format_string<Args...> fmt, Args&&... args) {
        instance().log(LogLevel::Trace, loc, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    static void debug(std::format_string<Args...> fmt, Args&&... args) {
        instance().log(LogLevel::Debug, std::source_location{}, fmt, std::forward<Args>(args)...);
    }
    template<typename... Args>
    static void debug(std::source_location loc, std::format_string<Args...> fmt, Args&&... args) {
        instance().log(LogLevel::Debug, loc, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    static void info(std::format_string<Args...> fmt, Args&&... args) {
        instance().log(LogLevel::Info, std::source_location{}, fmt, std::forward<Args>(args)...);
    }
    template<typename... Args>
    static void info(std::source_location loc, std::format_string<Args...> fmt, Args&&... args) {
        instance().log(LogLevel::Info, loc, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    static void warn(std::format_string<Args...> fmt, Args&&... args) {
        instance().log(LogLevel::Warn, std::source_location{}, fmt, std::forward<Args>(args)...);
    }
    template<typename... Args>
    static void warn(std::source_location loc, std::format_string<Args...> fmt, Args&&... args) {
        instance().log(LogLevel::Warn, loc, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    static void error(std::format_string<Args...> fmt, Args&&... args) {
        instance().log(LogLevel::Error, std::source_location{}, fmt, std::forward<Args>(args)...);
    }
    template<typename... Args>
    static void error(std::source_location loc, std::format_string<Args...> fmt, Args&&... args) {
        instance().log(LogLevel::Error, loc, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    static void fatal(std::format_string<Args...> fmt, Args&&... args) {
        instance().log(LogLevel::Fatal, std::source_location{}, fmt, std::forward<Args>(args)...);
    }
    template<typename... Args>
    static void fatal(std::source_location loc, std::format_string<Args...> fmt, Args&&... args) {
        instance().log(LogLevel::Fatal, loc, fmt, std::forward<Args>(args)...);
    }

private:
    GLog();
    ~GLog() = default;

    GLog(const GLog&) = delete;
    GLog& operator=(const GLog&) = delete;

    std::unique_ptr<ILogger> logger_;
    mutable std::mutex mutex_;
    LogLevel min_level_ = LogLevel::Info;
};

// 初始化/关闭（管理生命周期，非必须，但推荐）
void glog_initialize();
void glog_shutdown();

} // namespace gryce_engine::utils

// ---------------------------------------------------------------------------
// 宏封装（__VA_OPT__ 用于零参数兼容）
// ---------------------------------------------------------------------------
#define GLOG_TRACE(fmt, ...) gryce_engine::utils::GLog::trace(std::source_location::current(), fmt __VA_OPT__(,) __VA_ARGS__)
#define GLOG_DEBUG(fmt, ...) gryce_engine::utils::GLog::debug(std::source_location::current(), fmt __VA_OPT__(,) __VA_ARGS__)
#define GLOG_INFO(fmt, ...)  gryce_engine::utils::GLog::info(std::source_location::current(), fmt __VA_OPT__(,) __VA_ARGS__)
#define GLOG_WARN(fmt, ...)  gryce_engine::utils::GLog::warn(std::source_location::current(), fmt __VA_OPT__(,) __VA_ARGS__)
#define GLOG_ERROR(fmt, ...) gryce_engine::utils::GLog::error(std::source_location::current(), fmt __VA_OPT__(,) __VA_ARGS__)
#define GLOG_FATAL(fmt, ...) gryce_engine::utils::GLog::fatal(std::source_location::current(), fmt __VA_OPT__(,) __VA_ARGS__)
