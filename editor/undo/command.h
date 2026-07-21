#pragma once

#include <memory>
#include <string>

namespace gryce_engine::editor {

// ---------------------------------------------------------------------------
// EditorCommand — Undo/Redo 命令基类
// 每个具体命令负责：execute（redo）、undo、返回显示名称。
// 命令栈拥有命令所有权，调用 execute/undo 时场景必须有效。
// ---------------------------------------------------------------------------
class EditorCommand {
public:
    virtual ~EditorCommand() = default;

    // 执行命令（首次入栈时由 CommandStack 调用，也是 Redo 的实现）
    virtual void execute() = 0;

    // 撤销命令
    virtual void undo() = 0;

    // 用于菜单/日志的简短描述
    virtual std::string description() const = 0;
};

using EditorCommandPtr = std::unique_ptr<EditorCommand>;

} // namespace gryce_engine::editor
