#include "commands.h"

#include <algorithm>
#include <format>

#include "components/component.h"
#include "scene/scene.h"
#include "scene/scene_serializer.h"
#include "scene/entity.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::editor {

namespace {

// 从当前持有者（父实体或场景根列表）摘下实体所有权。
std::unique_ptr<scene::Entity> detach_entity(scene::Scene& scene, scene::Entity* entity) {
    if (scene::Entity* parent = entity->parent()) {
        return parent->detach_child(entity);
    }
    for (auto it = scene.roots().begin(); it != scene.roots().end(); ++it) {
        if (it->get() == entity) {
            std::unique_ptr<scene::Entity> owned = std::move(*it);
            scene.roots().erase(it);
            return owned;
        }
    }
    return nullptr;
}

// 把 detached 实体挂接到目标父实体下；parent=nullptr 表示挂到根级。
void attach_entity(scene::Scene& scene, std::unique_ptr<scene::Entity> entity,
                   scene::Entity* parent, size_t root_index = 0) {
    if (!entity) return;
    if (parent) {
        parent->add_child(std::move(entity));
    } else {
        auto& roots = scene.roots();
        if (root_index > roots.size()) root_index = roots.size();
        roots.insert(roots.begin() + static_cast<ptrdiff_t>(root_index),
                     std::move(entity));
    }
    scene.set_store_on_entity_for_all();
}

// 序列化实体子树并包装成完整场景 JSON，便于 SceneSerializer::deserialize 恢复。
nlohmann::json wrap_entity_subtree(const scene::Entity& entity) {
    nlohmann::json json;
    json["version"] = 1;
    json["name"] = "UndoBuffer";
    json["entities"] = nlohmann::json::array({scene::SceneSerializer::serialize_entity(entity)});
    return json;
}

// 反序列化单棵子树，返回根实体所有权（临时 scene 的 roots 会被清空）。
std::unique_ptr<scene::Entity> deserialize_subtree(const std::string& json_text) {
    try {
        nlohmann::json json = nlohmann::json::parse(json_text);
        auto temp = scene::SceneSerializer::deserialize(json);
        if (!temp || temp->roots().empty()) return nullptr;
        auto owned = std::move(temp->roots().front());
        temp->roots().clear();
        return owned;
    } catch (const std::exception& e) {
        GLOG_ERROR("Undo: failed to deserialize entity subtree: {}", e.what());
        return nullptr;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// ComponentFieldCommand
// ---------------------------------------------------------------------------
ComponentFieldCommand::ComponentFieldCommand(scene::Scene& scene,
                                             const scene::UUID& entity_uuid,
                                             const std::string& component_type,
                                             const std::string& field_name,
                                             FieldValue old_value,
                                             FieldValue new_value)
    : scene_(&scene),
      entity_uuid_(entity_uuid),
      component_type_(component_type),
      field_name_(field_name),
      old_value_(std::move(old_value)),
      new_value_(std::move(new_value)) {}

bool ComponentFieldCommand::apply_value(const FieldValue& value) {
    if (!scene_) return false;
    scene::Entity* entity = scene_->find_entity_by_uuid(entity_uuid_);
    if (!entity) return false;

    components::Component* component = entity->get_component_by_type(component_type_);
    if (!component) return false;

    const auto fields = reflection::Registry::instance().all_fields(component_type_);
    const reflection::FieldInfo* field = nullptr;
    for (const auto* f : fields) {
        if (f && f->name == field_name_) {
            field = f;
            break;
        }
    }
    if (!field || field->read_only || !field->write) return false;

    const bool ok = std::visit([&](const auto& v) -> bool {
        using T = std::decay_t<decltype(v)>;
        return reflection::write_field<T>(component, *field, v);
    }, value);

    return ok;
}

void ComponentFieldCommand::execute() {
    apply_value(new_value_);
}

void ComponentFieldCommand::undo() {
    apply_value(old_value_);
}

std::string ComponentFieldCommand::description() const {
    return std::format("Modify {}.{}", component_type_, field_name_);
}

// ---------------------------------------------------------------------------
// EntityRenameCommand
// ---------------------------------------------------------------------------
EntityRenameCommand::EntityRenameCommand(scene::Scene& scene,
                                         const scene::UUID& entity_uuid,
                                         std::string old_name,
                                         std::string new_name)
    : scene_(&scene),
      entity_uuid_(entity_uuid),
      old_name_(std::move(old_name)),
      new_name_(std::move(new_name)) {}

void EntityRenameCommand::set_name(const std::string& name) {
    if (!scene_) return;
    scene::Entity* entity = scene_->find_entity_by_uuid(entity_uuid_);
    if (entity) entity->set_name(name);
}

void EntityRenameCommand::execute() {
    set_name(new_name_);
}

void EntityRenameCommand::undo() {
    set_name(old_name_);
}

std::string EntityRenameCommand::description() const {
    return std::format("Rename '{}' -> '{}'", old_name_, new_name_);
}

// ---------------------------------------------------------------------------
// EntityDeleteCommand
// ---------------------------------------------------------------------------
EntityDeleteCommand::EntityDeleteCommand(scene::Scene& scene,
                                         const scene::UUID& entity_uuid)
    : scene_(&scene), entity_uuid_(entity_uuid), parent_uuid_(scene::UUID::nil()) {}

void EntityDeleteCommand::serialize_and_remove() {
    if (!scene_) return;
    scene::Entity* entity = scene_->find_entity_by_uuid(entity_uuid_);
    if (!entity) return;

    serialized_subtree_ = wrap_entity_subtree(*entity).dump();

    if (scene::Entity* parent = entity->parent()) {
        parent_uuid_ = parent->uuid();
        root_index_ = 0;
    } else {
        parent_uuid_ = scene::UUID::nil();
        const auto& roots = scene_->roots();
        for (size_t i = 0; i < roots.size(); ++i) {
            if (roots[i]->uuid() == entity_uuid_) {
                root_index_ = i;
                break;
            }
        }
    }

    scene_->destroy_entity(entity);
}

void EntityDeleteCommand::execute() {
    serialize_and_remove();
}

void EntityDeleteCommand::undo() {
    if (!scene_ || serialized_subtree_.empty()) return;

    auto entity = deserialize_subtree(serialized_subtree_);
    if (!entity) return;

    scene::Entity* parent = parent_uuid_.is_valid() ? scene_->find_entity_by_uuid(parent_uuid_)
                                                    : nullptr;
    attach_entity(*scene_, std::move(entity), parent, root_index_);
}

std::string EntityDeleteCommand::description() const {
    return "Delete entity";
}

// ---------------------------------------------------------------------------
// EntityCreateCommand
// ---------------------------------------------------------------------------
EntityCreateCommand::EntityCreateCommand(scene::Scene& scene,
                                         const std::string& name,
                                         const scene::UUID& parent_uuid)
    : scene_(&scene), name_(name), parent_uuid_(parent_uuid) {}

void EntityCreateCommand::execute() {
    if (!scene_) return;
    scene::Entity* entity = nullptr;
    if (parent_uuid_.is_valid()) {
        if (scene::Entity* parent = scene_->find_entity_by_uuid(parent_uuid_)) {
            auto child = std::make_unique<scene::Entity>(name_);
            scene::UUID id = child->uuid();
            parent->add_child(std::move(child));
            entity = scene_->find_entity_by_uuid(id);
        }
    }
    if (!entity) {
        entity = scene_->create_entity(name_);
    }
    created_uuid_ = entity->uuid();
}

void EntityCreateCommand::undo() {
    if (!scene_ || !created_uuid_.is_valid()) return;
    if (scene::Entity* entity = scene_->find_entity_by_uuid(created_uuid_)) {
        scene_->destroy_entity(entity);
    }
}

std::string EntityCreateCommand::description() const {
    return std::format("Create '{}'", name_);
}

// ---------------------------------------------------------------------------
// EntityReparentCommand
// ---------------------------------------------------------------------------
EntityReparentCommand::EntityReparentCommand(scene::Scene& scene,
                                             const scene::UUID& child_uuid,
                                             const scene::UUID& new_parent_uuid)
    : scene_(&scene),
      child_uuid_(child_uuid),
      new_parent_uuid_(new_parent_uuid),
      old_parent_uuid_(scene::UUID::nil()) {}

void EntityReparentCommand::reparent(const scene::UUID& parent_uuid) {
    if (!scene_) return;
    scene::Entity* child = scene_->find_entity_by_uuid(child_uuid_);
    if (!child) return;

    std::unique_ptr<scene::Entity> owned = detach_entity(*scene_, child);
    if (!owned) return;

    scene::Entity* parent = parent_uuid.is_valid() ? scene_->find_entity_by_uuid(parent_uuid)
                                                   : nullptr;
    attach_entity(*scene_, std::move(owned), parent, 0);
}

void EntityReparentCommand::execute() {
    if (!scene_) return;
    scene::Entity* child = scene_->find_entity_by_uuid(child_uuid_);
    if (!child) return;

    if (scene::Entity* parent = child->parent()) {
        old_parent_uuid_ = parent->uuid();
        old_root_index_ = 0;
    } else {
        old_parent_uuid_ = scene::UUID::nil();
        const auto& roots = scene_->roots();
        for (size_t i = 0; i < roots.size(); ++i) {
            if (roots[i]->uuid() == child_uuid_) {
                old_root_index_ = i;
                break;
            }
        }
    }

    reparent(new_parent_uuid_);
}

void EntityReparentCommand::undo() {
    if (!scene_) return;
    scene::Entity* child = scene_->find_entity_by_uuid(child_uuid_);
    if (!child) return;

    std::unique_ptr<scene::Entity> owned = detach_entity(*scene_, child);
    if (!owned) return;

    scene::Entity* parent = old_parent_uuid_.is_valid() ? scene_->find_entity_by_uuid(old_parent_uuid_)
                                                        : nullptr;
    attach_entity(*scene_, std::move(owned), parent, old_root_index_);
}

std::string EntityReparentCommand::description() const {
    return "Reparent entity";
}

// ---------------------------------------------------------------------------
// ComponentMultiFieldCommand
// ---------------------------------------------------------------------------
ComponentMultiFieldCommand::ComponentMultiFieldCommand(scene::Scene& scene,
                                                       const scene::UUID& entity_uuid,
                                                       const std::string& component_type,
                                                       std::vector<ComponentMultiFieldChange> changes)
    : scene_(&scene),
      entity_uuid_(entity_uuid),
      component_type_(component_type),
      changes_(std::move(changes)) {}

bool ComponentMultiFieldCommand::apply_value(const std::string& field_name,
                                             const FieldValue& value) {
    if (!scene_) return false;
    scene::Entity* entity = scene_->find_entity_by_uuid(entity_uuid_);
    if (!entity) return false;

    components::Component* component = entity->get_component_by_type(component_type_);
    if (!component) return false;

    const auto fields = reflection::Registry::instance().all_fields(component_type_);
    const reflection::FieldInfo* field = nullptr;
    for (const auto* f : fields) {
        if (f && f->name == field_name) {
            field = f;
            break;
        }
    }
    if (!field || field->read_only || !field->write) return false;

    const bool ok = std::visit([&](const auto& v) -> bool {
        using T = std::decay_t<decltype(v)>;
        return reflection::write_field<T>(component, *field, v);
    }, value);

    return ok;
}

void ComponentMultiFieldCommand::execute() {
    for (const auto& c : changes_) {
        apply_value(c.field_name, c.new_value);
    }
}

void ComponentMultiFieldCommand::undo() {
    for (const auto& c : changes_) {
        apply_value(c.field_name, c.old_value);
    }
}

std::string ComponentMultiFieldCommand::description() const {
    return std::format("Modify {} ({} fields)", component_type_, changes_.size());
}

} // namespace gryce_engine::editor
