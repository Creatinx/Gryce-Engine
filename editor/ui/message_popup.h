#pragma once

#include <string>
#include <vector>

namespace gryce_engine::editor {

// ---------------------------------------------------------------------------
// MessagePopup — 通用警告/错误弹窗
//
// 任何位置调用 warn()/error() 登记一条消息；EditorApp 主循环每帧调用
// draw() 以模态对话框显示。多条消息排队逐个显示。
// ---------------------------------------------------------------------------
class MessagePopup {
public:
    static MessagePopup& instance();

    void warn(const std::string& message);
    void error(const std::string& message);

    // 每帧调用一次（建议在所有面板绘制之后）
    void draw();

private:
    MessagePopup() = default;

    struct Item {
        std::string title_key; // 本地化 key（common.warning / common.error）
        std::string message;
    };

    std::vector<Item> queue_;
    bool open_requested_ = false;
};

} // namespace gryce_engine::editor
