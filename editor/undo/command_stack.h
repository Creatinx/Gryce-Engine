#pragma once

#include <memory>
#include <string>
#include <vector>

#include "command.h"

namespace gryce_engine::editor {

// ---------------------------------------------------------------------------
// CommandStack — 编辑器撤销/重做栈
// 标准双栈模型：undo_stack_ 保存已执行命令，redo_stack_ 保存被撤销命令。
// 新命令入栈时清空 redo 栈。
// ---------------------------------------------------------------------------
class CommandStack {
public:
    CommandStack() = default;
    ~CommandStack() = default;

    CommandStack(const CommandStack&) = delete;
    CommandStack& operator=(const CommandStack&) = delete;

    // 执行并 owning 一个新命令；push 后命令所有权归栈所有
    void push(EditorCommandPtr command);

    // 撤销上一个命令
    void undo();

    // 重做上一个被撤销的命令
    void redo();

    // 清空所有记录
    void clear();

    bool can_undo() const { return !undo_stack_.empty(); }
    bool can_redo() const { return !redo_stack_.empty(); }

    // 用于 UI 菜单显示
    std::string peek_undo_description() const;
    std::string peek_redo_description() const;

private:
    std::vector<EditorCommandPtr> undo_stack_;
    std::vector<EditorCommandPtr> redo_stack_;
};

} // namespace gryce_engine::editor
