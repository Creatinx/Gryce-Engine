#include "command_stack.h"

#include "utils/glog/glog_lib.h"

namespace gryce_engine::editor {

void CommandStack::push(EditorCommandPtr command) {
    if (!command) {
        GLOG_ERROR("CommandStack: push called with null command");
        return;
    }
    
    // 诊断：检查命令对象是否有效
    GLOG_DEBUG("CommandStack: pushing command at {}", static_cast<void*>(command.get()));
    
    try {
        command->execute();
    } catch (const std::exception& e) {
        GLOG_ERROR("CommandStack: command execution threw exception: {}", e.what());
        return;
    } catch (...) {
        GLOG_ERROR("CommandStack: command execution threw unknown exception");
        return;
    }
    
    // 诊断：检查命令对象在 execute 后是否仍然有效
    if (!command) {
        GLOG_ERROR("CommandStack: command became null after execute()");
        return;
    }
    
    bool has_failed = false;
    try {
        has_failed = command->failed();
    } catch (const std::exception& e) {
        GLOG_ERROR("CommandStack: failed() threw exception: {}", e.what());
        return;
    } catch (...) {
        GLOG_ERROR("CommandStack: failed() threw unknown exception");
        return;
    }
    
    if (has_failed) {
        std::string desc;
        try {
            desc = command->description();
        } catch (...) {
            desc = "<unknown>";
        }
        GLOG_INFO("CommandStack: command '{}' failed, not pushing to undo stack", desc);
        return;
    }
    
    undo_stack_.push_back(std::move(command));
    redo_stack_.clear();
}

void CommandStack::undo() {
    if (undo_stack_.empty()) {
        GLOG_INFO("CommandStack: undo requested but stack empty");
        return;
    }
    auto command = std::move(undo_stack_.back());
    GLOG_INFO("CommandStack: undo '{}'", command->description());
    undo_stack_.pop_back();
    command->undo();
    redo_stack_.push_back(std::move(command));
}

void CommandStack::redo() {
    if (redo_stack_.empty()) {
        GLOG_INFO("CommandStack: redo requested but stack empty");
        return;
    }
    auto command = std::move(redo_stack_.back());
    GLOG_INFO("CommandStack: redo '{}'", command->description());
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
