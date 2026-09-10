#include "command_stack.h"

#include "components/component_factory.h"

namespace gryce_engine::editor {

// ---------------------------------------------------------------------------
// 属性 JSON 读写
// ---------------------------------------------------------------------------
nlohmann::json read_property_json(const components::Component* component,
                                  const reflection::FieldInfo& field) {
    nlohmann::json out;
    if (!component) return out;

    switch (field.type) {
    case reflection::FieldType::Bool:
        out = reflection::read_field<bool>(component, field);
        break;
    case reflection::FieldType::Int:
    case reflection::FieldType::Enum:
        out = reflection::read_field<int>(component, field);
        break;
    case reflection::FieldType::Float:
        out = reflection::read_field<float>(component, field);
        break;
    case reflection::FieldType::Double:
        out = reflection::read_field<double>(component, field);
        break;
    case reflection::FieldType::String:
        out = reflection::read_field<std::string>(component, field);
        break;
    case reflection::FieldType::Vector2f: {
        auto v = reflection::read_field<math::Vector2f>(component, field);
        out = {v.x, v.y};
        break;
    }
    case reflection::FieldType::Vector3f: {
        auto v = reflection::read_field<math::Vector3f>(component, field);
        out = {v.x, v.y, v.z};
        break;
    }
    case reflection::FieldType::Vector3i: {
        auto v = reflection::read_field<math::Vector3i>(component, field);
        out = {v.x, v.y, v.z};
        break;
    }
    case reflection::FieldType::Vector4f: {
        auto v = reflection::read_field<math::Vector4f>(component, field);
        out = {v.x, v.y, v.z, v.w};
        break;
    }
    case reflection::FieldType::Quaternionf: {
        auto v = reflection::read_field<math::Quaternionf>(component, field);
        out = {v.x, v.y, v.z, v.w};
        break;
    }
    case reflection::FieldType::Color: {
        auto v = reflection::read_field<render::Color>(component, field);
        out = {v.r, v.g, v.b, v.a};
        break;
    }
    }
    return out;
}

bool write_property_json(components::Component* component,
                         const reflection::FieldInfo& field,
                         const nlohmann::json& value) {
    if (!component) return false;

    switch (field.type) {
    case reflection::FieldType::Bool:
        return reflection::write_field(component, field, value.get<bool>());
    case reflection::FieldType::Int:
    case reflection::FieldType::Enum:
        return reflection::write_field(component, field, value.get<int>());
    case reflection::FieldType::Float:
        return reflection::write_field(component, field, value.get<float>());
    case reflection::FieldType::Double:
        return reflection::write_field(component, field, value.get<double>());
    case reflection::FieldType::String:
        return reflection::write_field(component, field, value.get<std::string>());
    case reflection::FieldType::Vector2f:
        return reflection::write_field(
            component, field,
            math::Vector2f(value[0].get<float>(), value[1].get<float>()));
    case reflection::FieldType::Vector3f:
        return reflection::write_field(
            component, field,
            math::Vector3f(value[0].get<float>(), value[1].get<float>(),
                           value[2].get<float>()));
    case reflection::FieldType::Vector3i:
        return reflection::write_field(
            component, field,
            math::Vector3i(value[0].get<int>(), value[1].get<int>(),
                           value[2].get<int>()));
    case reflection::FieldType::Vector4f:
        return reflection::write_field(
            component, field,
            math::Vector4f(value[0].get<float>(), value[1].get<float>(),
                           value[2].get<float>(), value[3].get<float>()));
    case reflection::FieldType::Quaternionf:
        return reflection::write_field(
            component, field,
            math::Quaternionf(value[0].get<float>(), value[1].get<float>(),
                              value[2].get<float>(), value[3].get<float>()));
    case reflection::FieldType::Color:
        return reflection::write_field(
            component, field,
            render::Color(value[0].get<float>(), value[1].get<float>(),
                          value[2].get<float>(), value[3].get<float>()));
    }
    return false;
}

// ---------------------------------------------------------------------------
// CreateEntityCommand
// ---------------------------------------------------------------------------
CreateEntityCommand::CreateEntityCommand(scene::Scene* scene, std::string name,
                                         scene::Entity* parent)
    : scene_(scene), name_(std::move(name)), parent_(parent) {}

void CreateEntityCommand::execute() {
    if (!scene_) return;

    if (owned_) {
        if (parent_) {
            entity_ = parent_->add_child(std::move(owned_));
        } else {
            entity_ = scene_->add_root_entity(std::move(owned_));
        }
    } else {
        entity_ = scene_->create_entity(name_);
        if (parent_ && parent_ != entity_) {
            auto detached = scene_->root()->detach_child(entity_);
            if (detached) {
                parent_->add_child(std::move(detached));
            }
        }
    }

    if (entity_) entity_->mark_dirty();
    if (parent_) parent_->mark_dirty();
}

void CreateEntityCommand::undo() {
    if (!scene_ || !entity_) return;

    scene::Entity* owner = entity_->parent();
    if (owner) {
        owned_ = owner->detach_child(entity_);
    } else {
        owned_ = scene_->root()->detach_child(entity_);
    }
    if (owned_) {
        entity_ = owned_.get();
    }
    if (entity_) entity_->mark_dirty();
    if (parent_) parent_->mark_dirty();
}

// ---------------------------------------------------------------------------
// DeleteEntityCommand
// ---------------------------------------------------------------------------
DeleteEntityCommand::DeleteEntityCommand(scene::Scene* scene,
                                         scene::Entity* entity)
    : scene_(scene), entity_(entity), parent_(entity ? entity->parent() : nullptr) {}

void DeleteEntityCommand::execute() {
    if (!scene_ || !entity_) return;

    scene::Entity* owner = entity_->parent();
    if (owner) {
        owned_ = owner->detach_child(entity_);
    } else {
        owned_ = scene_->root()->detach_child(entity_);
    }
    if (owned_) {
        entity_ = owned_.get();
    }
    if (entity_) entity_->mark_dirty();
    if (parent_) parent_->mark_dirty();
}

void DeleteEntityCommand::undo() {
    if (!scene_ || !owned_) return;

    if (parent_) {
        entity_ = parent_->add_child(std::move(owned_));
    } else {
        entity_ = scene_->add_root_entity(std::move(owned_));
    }
    if (entity_) entity_->mark_dirty();
    if (parent_) parent_->mark_dirty();
}

// ---------------------------------------------------------------------------
// ReparentEntityCommand
// ---------------------------------------------------------------------------
ReparentEntityCommand::ReparentEntityCommand(scene::Scene* scene,
                                             scene::Entity* entity,
                                             scene::Entity* new_parent)
    : scene_(scene), entity_(entity), new_parent_(new_parent) {}

void ReparentEntityCommand::execute() {
    if (!scene_ || !entity_ || !new_parent_ || entity_ == new_parent_) return;

    old_parent_ = entity_->parent();
    if (!old_parent_) old_parent_ = scene_->root();

    if (entity_->parent()) {
        owned_ = entity_->parent()->detach_child(entity_);
    } else {
        owned_ = scene_->root()->detach_child(entity_);
    }
    if (!owned_) return;

    entity_ = new_parent_->add_child(std::move(owned_));
    if (entity_) entity_->mark_dirty();
    if (old_parent_) old_parent_->mark_dirty();
    if (new_parent_) new_parent_->mark_dirty();
}

void ReparentEntityCommand::undo() {
    if (!scene_ || !entity_ || !old_parent_) return;

    if (entity_->parent()) {
        owned_ = entity_->parent()->detach_child(entity_);
    } else {
        owned_ = scene_->root()->detach_child(entity_);
    }
    if (!owned_) return;

    entity_ = old_parent_->add_child(std::move(owned_));
    if (entity_) entity_->mark_dirty();
    if (old_parent_) old_parent_->mark_dirty();
    if (new_parent_) new_parent_->mark_dirty();
}

// ---------------------------------------------------------------------------
// AddComponentCommand
// ---------------------------------------------------------------------------
AddComponentCommand::AddComponentCommand(scene::Scene* scene,
                                         scene::Entity* entity,
                                         std::string type)
    : scene_(scene), entity_(entity), type_(std::move(type)) {}

void AddComponentCommand::execute() {
    if (!scene_ || !entity_) return;

    std::unique_ptr<components::Component> comp;
    if (!state_json_.is_null()) {
        comp = components::ComponentFactory::instance().create(type_);
        if (comp && state_json_.is_object()) {
            comp->deserialize(state_json_);
        }
    } else {
        comp = components::ComponentFactory::instance().create(type_);
    }
    if (!comp) return;

    component_ = entity_->add_component(std::move(comp));
    if (component_ && state_json_.is_null()) {
        component_->serialize(state_json_);
    }
    entity_->mark_dirty();
}

void AddComponentCommand::undo() {
    if (!entity_ || !component_) return;

    component_->serialize(state_json_);
    entity_->remove_component(component_);
    component_ = nullptr;
    entity_->mark_dirty();
}

// ---------------------------------------------------------------------------
// RemoveComponentCommand
// ---------------------------------------------------------------------------
RemoveComponentCommand::RemoveComponentCommand(scene::Scene* scene,
                                               scene::Entity* entity,
                                               components::Component* component)
    : scene_(scene), entity_(entity), component_(component) {
    if (component_) {
        type_ = component_->type();
    }
}

void RemoveComponentCommand::execute() {
    if (!entity_ || !component_) return;

    component_->serialize(state_json_);
    type_ = component_->type();
    entity_->remove_component(component_);
    component_ = nullptr;
    entity_->mark_dirty();
}

void RemoveComponentCommand::undo() {
    if (!entity_ || type_.empty()) return;

    auto comp = components::ComponentFactory::instance().create(type_);
    if (!comp) return;
    if (state_json_.is_object()) {
        comp->deserialize(state_json_);
    }
    component_ = entity_->add_component(std::move(comp));
    entity_->mark_dirty();
}

// ---------------------------------------------------------------------------
// ModifyPropertyCommand
// ---------------------------------------------------------------------------
ModifyPropertyCommand::ModifyPropertyCommand(
    scene::Scene* scene, components::Component* component,
    reflection::FieldInfo field, nlohmann::json old_value,
    nlohmann::json new_value)
    : scene_(scene), component_(component), field_(std::move(field)),
      old_value_(std::move(old_value)), new_value_(std::move(new_value)) {}

void ModifyPropertyCommand::execute() {
    if (!component_) return;
    write_property_json(component_, field_, new_value_);
    if (component_->owner()) component_->owner()->mark_dirty();
}

void ModifyPropertyCommand::undo() {
    if (!component_) return;
    write_property_json(component_, field_, old_value_);
    if (component_->owner()) component_->owner()->mark_dirty();
}

// ---------------------------------------------------------------------------
// CommandStack
// ---------------------------------------------------------------------------
Command* CommandStack::push(std::unique_ptr<Command> command) {
    if (!command) return nullptr;

    command->execute();
    if (undo_stack_.size() >= max_history_) {
        undo_stack_.erase(undo_stack_.begin());
    }
    Command* raw = command.get();
    undo_stack_.push_back(std::move(command));
    redo_stack_.clear();
    return raw;
}

void CommandStack::undo() {
    if (undo_stack_.empty()) return;
    auto command = std::move(undo_stack_.back());
    undo_stack_.pop_back();
    command->undo();
    redo_stack_.push_back(std::move(command));
}

void CommandStack::redo() {
    if (redo_stack_.empty()) return;
    auto command = std::move(redo_stack_.back());
    redo_stack_.pop_back();
    command->execute();
    undo_stack_.push_back(std::move(command));
}

void CommandStack::clear() {
    undo_stack_.clear();
    redo_stack_.clear();
}

} // namespace gryce_engine::editor
