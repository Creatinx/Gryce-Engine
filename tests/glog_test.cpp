#include <gtest/gtest.h>
#include "utils/glog/glog_lib.h"

using namespace gryce_engine::utils;

// ---------------------------------------------------------------------------
// GLog 分级输出
// ---------------------------------------------------------------------------
TEST(GLog, AllLevels) {
    glog_initialize();
    GLOG_TRACE("trace value {}", 1);
    GLOG_DEBUG("debug value {}", 2);
    GLOG_INFO("info value {}", 3);
    GLOG_WARN("warn value {}", 4);
    GLOG_ERROR("error value {}", 5);
    GLOG_INFO("message without format args");
    glog_shutdown();
}

// ---------------------------------------------------------------------------
// ILogger 接口可替换
// ---------------------------------------------------------------------------
class MockLogger : public ILogger {
public:
    std::string last_message;
    LogLevel last_level = LogLevel::Info;
    int call_count = 0;

    void log(LogLevel level, const std::string& message) override {
        last_level = level;
        last_message = message;
        ++call_count;
    }
};

TEST(GLog, SetCustomLogger) {
    glog_initialize();

    auto mock = std::make_unique<MockLogger>();
    MockLogger* raw = mock.get();
    GLog::instance().set_logger(std::move(mock));

    GLOG_INFO("test message {}", 42);
    EXPECT_EQ(raw->call_count, 1);
    EXPECT_EQ(raw->last_level, LogLevel::Info);
    EXPECT_EQ(raw->last_message, "test message 42");

    GLOG_WARN("warning");
    EXPECT_EQ(raw->call_count, 2);
    EXPECT_EQ(raw->last_level, LogLevel::Warn);
    EXPECT_EQ(raw->last_message, "warning");

    // 恢复默认 ConsoleLogger
    GLog::instance().set_logger(std::make_unique<ConsoleLogger>());
    glog_shutdown();
}

// ---------------------------------------------------------------------------
// 模板接口类型安全
// ---------------------------------------------------------------------------
TEST(GLog, TemplateTypeSafety) {
    glog_initialize();
    GLOG_INFO("int={}, float={}, string={}", 42, 3.14, "hello");
    glog_shutdown();
}

// ---------------------------------------------------------------------------
// 空格式参数兼容
// ---------------------------------------------------------------------------
TEST(GLog, NoFormatArgs) {
    glog_initialize();
    GLOG_INFO("plain text");
    glog_shutdown();
}
