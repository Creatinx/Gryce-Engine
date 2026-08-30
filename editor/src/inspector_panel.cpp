#include "inspector_panel.h"

#include <algorithm>
#include <array>
#include <cstring>

#include <imgui.h>

#include "components/component_factory.h"
#include "command_stack.h"
#include "hierarchy_panel.h"
#include "reflection/reflection.h"

namespace gryce_engine::editor {

scene::Entity* InspectorPanel::selected_entity() const {
    if (!hierarchy_) return nullptr;
    return hierarchy_->selected_entity();
}

void InspectorPanel::render() {
    ImGui::Begin("Inspector");

    scene::Entity* entity = selected_entity();
    if (!entity) {
        ImGui::TextUnformatted("Select an entity to inspect");
        ImGui::End();
        return;
    }

    render_entity_header(entity);

    ImGui::Separator();

    for (auto& component : entity->components()) {
        render_component(entity, component.get());
    }

    // 延迟删除组件，避免在遍历 components_ 时改变容器。
    for (components::Component* component : queued_component_removals_) {
        if (command_stack_) {
            command_stack_->push(
                std::make_unique<RemoveComponentCommand>(scene_, entity, component));
        } else {
            entity->remove_component(component);
        }
    }
    queued_component_removals_.clear();

    draw_add_component_popup(entity);

    ImGui::End();
}

void InspectorPanel::render_entity_header(scene::Entity* entity) {
    static std::array<char, 256> name_buffer{};
    std::fill(name_buffer.begin(), name_buffer.end(), '\0');
    std::strncpy(name_buffer.data(), entity->name().c_str(), name_buffer.size() - 1);

    ImGui::TextUnformatted("Entity");
    ImGui::SameLine();
    ImGui::BeginDisabled(play_mode_);
    if (ImGui::InputText("##entity_name", name_buffer.data(), name_buffer.size())) {
        entity->set_name(name_buffer.data());
    }
    ImGui::EndDisabled();

    ImGui::Text("UUID: %s", entity->uuid().str().c_str());
    ImGui::BeginDisabled(play_mode_);
    ImGui::Checkbox("Enabled", &entity->enabled);
    if (ImGui::IsItemEdited()) {
        entity->mark_dirty();
    }
    ImGui::EndDisabled();
}

void InspectorPanel::render_component(scene::Entity* entity, components::Component* component) {
    if (!component) return;

    ImGui::PushID(component);
    bool open = ImGui::CollapsingHeader(component->type(),
                                        ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::SameLine();
    ImGui::BeginDisabled(play_mode_);
    if (ImGui::SmallButton("x")) {
        queue_remove_component(component);
    }
    ImGui::EndDisabled();
    ImGui::PopID();

    if (!open) return;

    ImGui::PushID(component);
    const auto* type_info = reflection::Registry::instance().find(component->type());
    if (!type_info) {
        ImGui::TextDisabled("No reflection metadata for %s", component->type());
        ImGui::PopID();
        return;
    }

    for (const reflection::FieldInfo* field :
         reflection::Registry::instance().all_fields(component->type())) {
        if (!field) continue;
        render_field(component, *field);
    }
    ImGui::PopID();
}

void InspectorPanel::render_field(components::Component* component,
                                  const reflection::FieldInfo& field) {
    ImGui::PushID(field.name.c_str());
    const bool read_only = is_field_read_only(component, field);
    ImGui::BeginDisabled(read_only);

    nlohmann::json old_json;
    if (command_stack_ && !read_only) {
        old_json = read_property_json(component, field);
    }

    bool changed = false;
    switch (field.type) {
    case reflection::FieldType::Bool: {
        bool value = reflection::read_field<bool>(component, field);
        changed = ImGui::Checkbox(field.display_name.c_str(), &value);
        if (changed) reflection::write_field(component, field, value);
        break;
    }
    case reflection::FieldType::Int: {
        int value = reflection::read_field<int>(component, field);
        if (field.has_range) {
            changed = ImGui::SliderInt(field.display_name.c_str(), &value,
                                       static_cast<int>(field.range_min),
                                       static_cast<int>(field.range_max));
        } else {
            changed = ImGui::DragInt(field.display_name.c_str(), &value, 0.1f);
        }
        if (changed) reflection::write_field(component, field, value);
        break;
    }
    case reflection::FieldType::Float: {
        float value = reflection::read_field<float>(component, field);
        if (field.has_range) {
            changed = ImGui::SliderFloat(field.display_name.c_str(), &value,
                                         field.range_min, field.range_max);
        } else {
            changed = ImGui::DragFloat(field.display_name.c_str(), &value, 0.1f);
        }
        if (changed) reflection::write_field(component, field, value);
        break;
    }
    case reflection::FieldType::Double: {
        double value = reflection::read_field<double>(component, field);
        changed = ImGui::InputDouble(field.display_name.c_str(), &value);
        if (changed) reflection::write_field(component, field, value);
        break;
    }
    case reflection::FieldType::String: {
        std::string value = reflection::read_field<std::string>(component, field);
        std::array<char, 512> buffer{};
        std::fill(buffer.begin(), buffer.end(), '\0');
        std::strncpy(buffer.data(), value.c_str(), buffer.size() - 1);
        if (ImGui::InputText(field.display_name.c_str(), buffer.data(), buffer.size())) {
            reflection::write_field(component, field, std::string(buffer.data()));
            changed = true;
        }
        break;
    }
    case reflection::FieldType::Vector2f: {
        math::Vector2f value = reflection::read_field<math::Vector2f>(component, field);
        changed = ImGui::DragFloat2(field.display_name.c_str(), &value.x, 0.1f);
        if (changed) reflection::write_field(component, field, value);
        break;
    }
    case reflection::FieldType::Vector3f: {
        math::Vector3f value = reflection::read_field<math::Vector3f>(component, field);
        if (field.is_color) {
            changed = ImGui::ColorEdit3(field.display_name.c_str(), &value.x);
        } else {
            changed = ImGui::DragFloat3(field.display_name.c_str(), &value.x, 0.1f);
        }
        if (changed) reflection::write_field(component, field, value);
        break;
    }
    case reflection::FieldType::Vector3i: {
        math::Vector3i value = reflection::read_field<math::Vector3i>(component, field);
        int v[3] = {value.x, value.y, value.z};
        changed = ImGui::DragInt3(field.display_name.c_str(), v, 0.1f);
        if (changed) {
            value = math::Vector3i(v[0], v[1], v[2]);
            reflection::write_field(component, field, value);
        }
        break;
    }
    case reflection::FieldType::Vector4f: {
        math::Vector4f value = reflection::read_field<math::Vector4f>(component, field);
        if (field.is_color) {
            changed = ImGui::ColorEdit4(field.display_name.c_str(), &value.x,
                                        ImGuiColorEditFlags_AlphaBar);
        } else {
            changed = ImGui::DragFloat4(field.display_name.c_str(), &value.x, 0.1f);
        }
        if (changed) reflection::write_field(component, field, value);
        break;
    }
    case reflection::FieldType::Quaternionf: {
        math::Quaternionf value = reflection::read_field<math::Quaternionf>(component, field);
        math::Vector3f euler = value.to_euler();
        changed = ImGui::DragFloat3(field.display_name.c_str(), &euler.x, 0.5f);
        if (changed) {
            reflection::write_field(component, field,
                                    math::Quaternionf::from_euler(euler.x, euler.y, euler.z));
        }
        break;
    }
    case reflection::FieldType::Color: {
        render::Color value = reflection::read_field<render::Color>(component, field);
        changed = ImGui::ColorEdit4(field.display_name.c_str(), &value.r,
                                    ImGuiColorEditFlags_AlphaBar);
        if (changed) reflection::write_field(component, field, value);
        break;
    }
    case reflection::FieldType::Enum: {
        int value = reflection::read_field<int>(component, field);
        changed = ImGui::DragInt(field.display_name.c_str(), &value, 0.1f);
        if (changed) reflection::write_field(component, field, value);
        break;
    }
    }

    ImGui::EndDisabled();
    if (changed && component->owner()) {
        component->owner()->mark_dirty();
        if (command_stack_) {
            const nlohmann::json new_json =
                read_property_json(component, field);
            command_stack_->push(std::make_unique<ModifyPropertyCommand>(
                scene_, component, field, std::move(old_json), new_json));
        }
    }
    ImGui::PopID();
}

void InspectorPanel::draw_add_component_popup(scene::Entity* entity) {
    ImGui::BeginDisabled(play_mode_);
    if (ImGui::Button("Add Component")) {
        ImGui::OpenPopup("add_component_popup");
    }
    ImGui::EndDisabled();

    if (!ImGui::BeginPopup("add_component_popup")) return;

    static char filter[128] = {};
    ImGui::InputTextWithHint("##component_filter", "Search...", filter, sizeof(filter));

    ImGui::BeginChild("add_component_list", ImVec2(0.0f, 320.0f));
    for (const std::string& type : components::ComponentFactory::instance().all_types()) {
        if (is_internal_component_type(type)) continue;
        if (entity->get_component_by_type(type)) continue;
        if (!filter[0]) {
            // 空过滤时显示全部。
        } else if (type.find(filter) == std::string::npos) {
            continue;
        }

        const char* description = components::ComponentFactory::instance().description(type);
        const std::string label = type + "##add_" + type;
        if (ImGui::MenuItem(label.c_str())) {
            if (command_stack_) {
                command_stack_->push(
                    std::make_unique<AddComponentCommand>(scene_, entity, type));
                ImGui::CloseCurrentPopup();
            } else {
                auto component = components::ComponentFactory::instance().create(type);
                if (component) {
                    entity->add_component(std::move(component));
                    ImGui::CloseCurrentPopup();
                }
            }
        }
        if (ImGui::IsItemHovered() && description && description[0]) {
            ImGui::SetTooltip("%s", description);
        }
    }
    ImGui::EndChild();
    ImGui::EndPopup();
}

void InspectorPanel::queue_remove_component(components::Component* component) {
    if (!component) return;
    if (std::find(queued_component_removals_.begin(),
                  queued_component_removals_.end(),
                  component) == queued_component_removals_.end()) {
        queued_component_removals_.push_back(component);
    }
}

bool InspectorPanel::is_field_read_only(
    const components::Component* component,
    const reflection::FieldInfo& field) const {
    if (field.read_only || play_mode_) return true;

    // 计划要求：Camera 的 FOV / Near / Far 在 Inspector 中只读。
    if (component && std::string(component->type()) == "Camera") {
        return field.name == "fov" || field.name == "near_plane" ||
               field.name == "far_plane";
    }
    return false;
}

bool InspectorPanel::is_internal_component_type(const std::string& type) const {
    return type == "Transform" || type == "ParentComponent" ||
           type == "ChildrenComponent" || type == "PrefabInstance";
}

} // namespace gryce_engine::editor
