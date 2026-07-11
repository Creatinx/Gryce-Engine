#pragma once

#include <string>
#include <memory>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <iostream>
#include <mutex>
#include <format>

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
    void flush() override;
    bool supports_color() const override { return ansi_enabled_; }

private:
    bool ansi_enabled_ = false;
    const char* ansi_color(LogLevel level) const;
    static const char* level_str(LogLevel level);
    static std::string timestamp();
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
    void log(LogLevel level, std::format_string<Args...> fmt, Args&&... args) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!logger_ || static_cast<int>(level) < static_cast<int>(min_level_)) return;
        std::string msg = std::format(fmt, std::forward<Args>(args)...);
        logger_->log(level, msg);
    }

    // 静态便捷方法（直接调用单例）
    template<typename... Args>
    static void trace(std::format_string<Args...> fmt, Args&&... args) {
        instance().log(LogLevel::Trace, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    static void debug(std::format_string<Args...> fmt, Args&&... args) {
        instance().log(LogLevel::Debug, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    static void info(std::format_string<Args...> fmt, Args&&... args) {
        instance().log(LogLevel::Info, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    static void warn(std::format_string<Args...> fmt, Args&&... args) {
        instance().log(LogLevel::Warn, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    static void error(std::format_string<Args...> fmt, Args&&... args) {
        instance().log(LogLevel::Error, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    static void fatal(std::format_string<Args...> fmt, Args&&... args) {
        instance().log(LogLevel::Fatal, fmt, std::forward<Args>(args)...);
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
#define GLOG_TRACE(fmt, ...) gryce_engine::utils::GLog::trace(fmt __VA_OPT__(,) __VA_ARGS__)
#define GLOG_DEBUG(fmt, ...) gryce_engine::utils::GLog::debug(fmt __VA_OPT__(,) __VA_ARGS__)
#define GLOG_INFO(fmt, ...)  gryce_engine::utils::GLog::info(fmt __VA_OPT__(,) __VA_ARGS__)
#define GLOG_WARN(fmt, ...)  gryce_engine::utils::GLog::warn(fmt __VA_OPT__(,) __VA_ARGS__)
#define GLOG_ERROR(fmt, ...) gryce_engine::utils::GLog::error(fmt __VA_OPT__(,) __VA_ARGS__)
#define GLOG_FATAL(fmt, ...) gryce_engine::utils::GLog::fatal(fmt __VA_OPT__(,) __VA_ARGS__)
