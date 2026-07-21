#include "command_stack.h"

#include "utils/glog/glog_lib.h"

namespace gryce_engine::editor {

void CommandStack::push(EditorCommandPtr command) {
    if (!command) return;
    command->execute();
    undo_stack_.push_back(std::move(command));
    redo_stack_.clear();
}

void CommandStack::undo() {
    if (undo_stack_.empty()) return;
    EditorCommandPtr command = std::move(undo_stack_.back());
    undo_stack_.pop_back();
    command->undo();
    redo_stack_.push_back(std::move(command));
}

void CommandStack::redo() {
    if (redo_stack_.empty()) return;
    EditorCommandPtr command = std::move(redo_stack_.back());
    redo_stack_.pop_back();
    command->execute();
    undo_stack_.push_back(std::move(command));
}

void CommandStack::clear() {
    undo_stack_.clear();
    redo_stack_.clear();
}

std::string CommandStack::peek_undo_description() const {
    if (undo_stack_.empty()) return {};
    return undo_stack_.back()->description();
}

std::string CommandStack::peek_redo_description() const {
    if (redo_stack_.empty()) return {};
    return redo_stack_.back()->description();
}

} // namespace gryce_engine::editor
