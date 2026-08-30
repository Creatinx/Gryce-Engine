#pragma once

#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "components/component.h"
#include "reflection/reflection.h"
#include "scene/scene.h"

namespace gryce_engine::editor {

// ---------------------------------------------------------------------------
// Command — Undo/Redo 命令基类
// ---------------------------------------------------------------------------
class Command {
public:
    virtual ~Command() = default;
    virtual void execute() = 0;
    virtual void undo() = 0;
    virtual const char* name() const = 0;
};

// 属性字段读写 JSON 辅助（Inspector 与 ModifyPropertyCommand 共用）
nlohmann::json read_property_json(const components::Component* component,
                                  const reflection::FieldInfo& field);
bool write_property_json(components::Component* component,
                         const reflection::FieldInfo& field,
                         const nlohmann::json& value);

// ---------------------------------------------------------------------------
// 基础场景命令
// ---------------------------------------------------------------------------
class CreateEntityCommand : public Command {
public:
    CreateEntityCommand(scene::Scene* scene, std::string name,
                        scene::Entity* parent);
    void execute() override;
    void undo() override;
    const char* name() const override { return "Create Entity"; }
    scene::Entity* created_entity() const { return entity_; }

private:
    scene::Scene* scene_;
    std::string name_;
    scene::Entity* parent_;
    scene::Entity* entity_ = nullptr;
    std::unique_ptr<scene::Entity> owned_;
};

class DeleteEntityCommand : public Command {
public:
    DeleteEntityCommand(scene::Scene* scene, scene::Entity* entity);
    void execute() override;
    void undo() override;
    const char* name() const override { return "Delete Entity"; }

private:
    scene::Scene* scene_;
    scene::Entity* entity_ = nullptr;
    scene::Entity* parent_ = nullptr;
    std::unique_ptr<scene::Entity> owned_;
};

class ReparentEntityCommand : public Command {
public:
    ReparentEntityCommand(scene::Scene* scene, scene::Entity* entity,
                          scene::Entity* new_parent);
    void execute() override;
    void undo() override;
    const char* name() const override { return "Reparent Entity"; }

private:
    scene::Scene* scene_;
    scene::Entity* entity_ = nullptr;
    scene::Entity* new_parent_ = nullptr;
    scene::Entity* old_parent_ = nullptr;
    std::unique_ptr<scene::Entity> owned_;
};

class AddComponentCommand : public Command {
public:
    AddComponentCommand(scene::Scene* scene, scene::Entity* entity,
                        std::string type);
    void execute() override;
    void undo() override;
    const char* name() const override { return "Add Component"; }

private:
    scene::Scene* scene_;
    scene::Entity* entity_;
    std::string type_;
    components::Component* component_ = nullptr;
    nlohmann::json state_json_;
};

class RemoveComponentCommand : public Command {
public:
    RemoveComponentCommand(scene::Scene* scene, scene::Entity* entity,
                           components::Component* component);
    void execute() override;
    void undo() override;
    const char* name() const override { return "Remove Component"; }

private:
    scene::Scene* scene_;
    scene::Entity* entity_;
    std::string type_;
    components::Component* component_ = nullptr;
    nlohmann::json state_json_;
};

class ModifyPropertyCommand : public Command {
public:
    ModifyPropertyCommand(scene::Scene* scene, components::Component* component,
                          reflection::FieldInfo field,
                          nlohmann::json old_value,
                          nlohmann::json new_value);
    void execute() override;
    void undo() override;
    const char* name() const override { return "Modify Property"; }

private:
    scene::Scene* scene_;
    components::Component* component_;
    reflection::FieldInfo field_;
    nlohmann::json old_value_;
    nlohmann::json new_value_;
};

// ---------------------------------------------------------------------------
// CommandStack — 有限历史 Undo/Redo 栈
// ---------------------------------------------------------------------------
class CommandStack {
public:
    explicit CommandStack(size_t max_history = 100)
        : max_history_(max_history) {}

    Command* push(std::unique_ptr<Command> command);
    void undo();
    void redo();
    void clear();

    bool can_undo() const { return !undo_stack_.empty(); }
    bool can_redo() const { return !redo_stack_.empty(); }
    const char* undo_label() const {
        return undo_stack_.empty() ? "" : undo_stack_.back()->name();
    }
    const char* redo_label() const {
        return redo_stack_.empty() ? "" : redo_stack_.back()->name();
    }

private:
    size_t max_history_;
    std::vector<std::unique_ptr<Command>> undo_stack_;
    std::vector<std::unique_ptr<Command>> redo_stack_;
};

} // namespace gryce_engine::editor
