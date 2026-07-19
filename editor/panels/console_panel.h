#pragma once

#include "../editor_panel.h"

#include "utils/glog/glog_lib.h"

namespace gryce_engine::editor {

// ---------------------------------------------------------------------------
// ConsolePanel — 日志面板（M1-E1）
// 读取 MemoryLogSink 的环形缓冲，按级别过滤显示；支持自动滚动与清空。
// ---------------------------------------------------------------------------
class ConsolePanel : public EditorPanel {
public:
    ConsolePanel() : EditorPanel("Console") {}

protected:
    void on_imgui() override;

private:
    bool passes_filter(utils::LogLevel level) const;

    bool show_trace_ = false;
    bool show_debug_ = true;
    bool show_info_ = true;
    bool show_warn_ = true;
    bool show_error_ = true;
    bool auto_scroll_ = true;
};

} // namespace gryce_engine::editor
