#pragma once

#include <memory>
#include <string>
#include <vector>

#include "command.h"
#include "field_value.h"

#include "reflection/reflection.h"
#include "scene/uuid.h"

namespace gryce_engine {
namespace scene { class Entity; class Scene; }
namespace components { class Component; }
} // namespace gryce_engine

namespace gryce_engine::editor {

// ---------------------------------------------------------------------------
// ComponentFieldCommand — 通用组件字段修改（支持所有 reflection 字段类型）
// ---------------------------------------------------------------------------
class ComponentFieldCommand : public EditorCommand {
public:
    ComponentFieldCommand(scene::Scene& scene,
                          const scene::UUID& entity_uuid,
                          const std::string& component_type,
                          const std::string& field_name,
                          FieldValue old_value,
                          FieldValue new_value);

    void execute() override;
    void undo() override;
    std::string description() const override;

private:
    scene::Scene* scene_;
    scene::UUID entity_uuid_;
    std::string component_type_;
    std::string field_name_;
    FieldValue old_value_;
    FieldValue new_value_;

    bool apply_value(const FieldValue& value);
};

// ---------------------------------------------------------------------------
// EntityRenameCommand — 实体重命名
// ---------------------------------------------------------------------------
class EntityRenameCommand : public EditorCommand {
public:
    EntityRenameCommand(scene::Scene& scene,
                        const scene::UUID& entity_uuid,
                        std::string old_name,
                        std::string new_name);

    void execute() override;
    void undo() override;
    std::string description() const override;

private:
    scene::Scene* scene_;
    scene::UUID entity_uuid_;
    std::string old_name_;
    std::string new_name_;

    void set_name(const std::string& name);
};

// ---------------------------------------------------------------------------
// EntityDeleteCommand — 删除实体（含子树）
// 撤销时通过反序列化整个子树恢复。
// ---------------------------------------------------------------------------
class EntityDeleteCommand : public EditorCommand {
public:
    EntityDeleteCommand(scene::Scene& scene,
                        const scene::UUID& entity_uuid);

    void execute() override;
    void undo() override;
    std::string description() const override;

private:
    scene::Scene* scene_;
    scene::UUID entity_uuid_;
    std::string serialized_subtree_;
    scene::UUID parent_uuid_; // 删除时的父实体；nil 表示根级
    size_t root_index_ = 0;   // 根级时记录在原 roots 中的索引

    void serialize_and_remove();
};

// ---------------------------------------------------------------------------
// EntityCreateCommand — 创建空实体
// ---------------------------------------------------------------------------
class EntityCreateCommand : public EditorCommand {
public:
    EntityCreateCommand(scene::Scene& scene,
                        const std::string& name,
                        const scene::UUID& parent_uuid = scene::UUID::nil());

    void execute() override;
    void undo() override;
    std::string description() const override;

    scene::UUID created_uuid() const { return created_uuid_; }

private:
    scene::Scene* scene_;
    std::string name_;
    scene::UUID parent_uuid_;
    scene::UUID created_uuid_;
};

// ---------------------------------------------------------------------------
// ComponentMultiFieldCommand — 一次修改同一组件的多个字段
// 用于物理材质预设、动画关键帧等需要同时修改多个字段的场景。
// ---------------------------------------------------------------------------
struct ComponentMultiFieldChange {
    std::string field_name;
    FieldValue old_value;
    FieldValue new_value;
};

class ComponentMultiFieldCommand : public EditorCommand {
public:
    ComponentMultiFieldCommand(scene::Scene& scene,
                               const scene::UUID& entity_uuid,
                               const std::string& component_type,
                               std::vector<ComponentMultiFieldChange> changes);

    void execute() override;
    void undo() override;
    std::string description() const override;

private:
    scene::Scene* scene_;
    scene::UUID entity_uuid_;
    std::string component_type_;
    std::vector<ComponentMultiFieldChange> changes_;

    bool apply_value(const std::string& field_name, const FieldValue& value);
};

// ---------------------------------------------------------------------------
// EntityReparentCommand — 拖拽换父
// ---------------------------------------------------------------------------
class EntityReparentCommand : public EditorCommand {
public:
    EntityReparentCommand(scene::Scene& scene,
                          const scene::UUID& child_uuid,
                          const scene::UUID& new_parent_uuid);

    void execute() override;
    void undo() override;
    std::string description() const override;

private:
    scene::Scene* scene_;
    scene::UUID child_uuid_;
    scene::UUID new_parent_uuid_;
    scene::UUID old_parent_uuid_;
    size_t old_root_index_ = 0;

    void reparent(const scene::UUID& parent_uuid);
};

} // namespace gryce_engine::editor
