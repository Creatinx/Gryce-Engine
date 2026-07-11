#include "glog_lib.h"

namespace gryce_engine::utils {

// ---------------------------------------------------------------------------
// ConsoleLogger
// ---------------------------------------------------------------------------
ConsoleLogger::ConsoleLogger() {
    // 默认关闭 ANSI 颜色，避免在终端模拟器/日志文件中留下转义序列
    ansi_enabled_ = false;
#if defined(_WIN32)
    // 不再调用 win32_enable_vt()，保持输出干净
#endif
}

const char* ConsoleLogger::ansi_color(LogLevel level) const {
    if (!ansi_enabled_) return "";
    switch (level) {
        case LogLevel::Trace: return "\033[37m"; // 白色
        case LogLevel::Debug: return "\033[37m"; // 白色
        case LogLevel::Info:  return "\033[90m"; // 灰色
        case LogLevel::Warn:  return "\033[33m"; // 黄色
        case LogLevel::Error: return "\033[31m"; // 红色
        case LogLevel::Fatal: return "\033[31m"; // 红色
    }
    return "\033[37m";
}

const char* ConsoleLogger::level_str(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
    }
    return "UNKWN";
}

std::string ConsoleLogger::timestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &time_t_now);
#else
    localtime_r(&time_t_now, &local);
#endif
    std::ostringstream oss;
    oss << std::setfill('0')
        << std::setw(4) << (local.tm_year + 1900) << '/'
        << std::setw(2) << (local.tm_mon + 1) << '/'
        << std::setw(2) << local.tm_mday << '/'
        << std::setw(2) << local.tm_hour << '/'
        << std::setw(2) << local.tm_min << '/'
        << std::setw(2) << local.tm_sec;
    return oss.str();
}

void ConsoleLogger::log(LogLevel level, const std::string& message) {
    const char* color = ansi_color(level);
    const char* reset = ansi_enabled_ ? "\033[0m" : "";
    const char* name  = level_str(level);
    std::string ts    = timestamp();

    std::cerr << color
              << '[' << ts << ']'
              << '[' << name << ']'
              << '[' << message << ']'
              << reset
              << '\n';
}

void ConsoleLogger::flush() {
    std::cerr.flush();
}

// ---------------------------------------------------------------------------
// GLog 单例
// ---------------------------------------------------------------------------
GLog::GLog() : logger_(std::make_unique<ConsoleLogger>()) {}

GLog& GLog::instance() {
    static GLog inst;
    return inst;
}

void GLog::set_logger(std::unique_ptr<ILogger> logger) {
    std::lock_guard<std::mutex> lock(mutex_);
    logger_ = std::move(logger);
}

ILogger* GLog::logger() const {
    return logger_.get();
}

void GLog::set_min_level(LogLevel level) {
    std::lock_guard<std::mutex> lock(mutex_);
    min_level_ = level;
}

LogLevel GLog::min_level() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return min_level_;
}

void GLog::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (logger_) logger_->flush();
}

// ---------------------------------------------------------------------------
// 初始化/关闭
// ---------------------------------------------------------------------------
void glog_initialize() {
    // 单例在首次调用时自动构造，此处可扩展额外初始化
}

void glog_shutdown() {
    GLog::instance().flush();
}

} // namespace gryce_engine::utils
