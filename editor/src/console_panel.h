#pragma once

#include <string>

#include "utils/glog/glog_lib.h"

namespace gryce_engine::editor {

// ---------------------------------------------------------------------------
// ConsolePanel — 显示 MemoryLogSink 中的引擎日志
// 提供日志级别过滤，默认只显示 Warn 及以上级别。
// ---------------------------------------------------------------------------
class ConsolePanel {
public:
    void render();

private:
    utils::LogLevel min_display_level_ = utils::LogLevel::Warn;
    bool auto_scroll_ = true;
    char filter_[256] = {};
};

} // namespace gryce_engine::editor