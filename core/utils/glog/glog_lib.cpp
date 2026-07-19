#include "glog_lib.h"

#ifndef _WIN32
#include <unistd.h>
#endif

namespace gryce_engine::utils {

// ---------------------------------------------------------------------------
// ConsoleLogger
// ---------------------------------------------------------------------------
ConsoleLogger::ConsoleLogger() {
    ansi_enabled_ = false;
#if defined(_WIN32)
    // 尝试启用 Windows 控制台 VT 序列；不支持时保持无颜色，避免输出乱码
    HANDLE h_err = GetStdHandle(STD_ERROR_HANDLE);
    DWORD mode = 0;
    if (h_err != INVALID_HANDLE_VALUE && GetConsoleMode(h_err, &mode)) {
        mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        if (SetConsoleMode(h_err, mode)) {
            ansi_enabled_ = true;
        }
    }
#else
    ansi_enabled_ = isatty(STDERR_FILENO) != 0;
#endif
}

const char* ConsoleLogger::ansi_color(LogLevel level) const {
    if (!ansi_enabled_) return "";
    switch (level) {
        case LogLevel::Trace: return "\033[37m"; // 白色
        case LogLevel::Debug: return "\033[37m"; // 白色
        case LogLevel::Info:  return "\033[97m"; // 亮白色
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
// MemoryLogSink
// ---------------------------------------------------------------------------
MemoryLogSink::MemoryLogSink(std::unique_ptr<ILogger> inner, size_t capacity)
    : inner_(std::move(inner)), buffer_(capacity > 0 ? capacity : 1) {}

void MemoryLogSink::log(LogLevel level, const std::string& message) {
    // 先转发给原后端，保持控制台/文件输出行为不变
    if (inner_) inner_->log(level, message);

    std::lock_guard<std::mutex> lock(mutex_);
    LogEntry& slot = buffer_[head_];
    slot.level = level;
    slot.message = message;
    slot.timestamp = make_timestamp();
    head_ = (head_ + 1) % buffer_.size();
    if (count_ < buffer_.size()) ++count_;
}

void MemoryLogSink::flush() {
    if (inner_) inner_->flush();
}

bool MemoryLogSink::supports_color() const {
    return inner_ && inner_->supports_color();
}

std::vector<LogEntry> MemoryLogSink::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<LogEntry> out;
    out.reserve(count_);
    // 缓冲未满：最旧条目在 0；已满：最旧条目在 head_
    const size_t oldest = (count_ < buffer_.size()) ? 0 : head_;
    for (size_t i = 0; i < count_; ++i) {
        out.push_back(buffer_[(oldest + i) % buffer_.size()]);
    }
    return out;
}

void MemoryLogSink::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    head_ = 0;
    count_ = 0;
}

size_t MemoryLogSink::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_;
}

MemoryLogSink* MemoryLogSink::from_glog() {
    return dynamic_cast<MemoryLogSink*>(GLog::instance().logger());
}

const char* MemoryLogSink::level_str(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
    }
    return "UNKWN";
}

std::string MemoryLogSink::make_timestamp() {
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
        << std::setw(2) << local.tm_hour << ':'
        << std::setw(2) << local.tm_min << ':'
        << std::setw(2) << local.tm_sec;
    return oss.str();
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
