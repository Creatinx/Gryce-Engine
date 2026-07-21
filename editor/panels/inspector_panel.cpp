#include "inspector_panel.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <string>
#include <vector>

#include "components/component.h"
#include "components/mesh_renderer.h"
#include "components/skinned_mesh_renderer.h"
#include "components/physical_material.h"
#include "components/terrain.h"
#include "components/2d/particle_emitter.h"
#include "reflection/reflection.h"
#include "scene/entity.h"
#include "scene/scene.h"
#include "../localization/localization.h"
#include "../undo/commands.h"

namespace {

std::vector<std::string> split_string(const std::string& s, char delimiter) {
    std::vector<std::string> parts;
    std::string current;
    for (char c : s) {
        if (c == delimiter) {
            if (!current.empty()) {
                parts.push_back(std::move(current));
                current.clear();
            }
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        parts.push_back(std::move(current));
    }
    return parts;
}

using gryce_engine::editor::FieldValue;
using gryce_engine::reflection::FieldType;
using gryce_engine::reflection::FieldInfo;
using gryce_engine::reflection::read_field;

FieldValue read_field_value(gryce_engine::components::Component* component,
                            const FieldInfo& field) {
    switch (field.type) {
        case FieldType::Int:       return read_field<int>(component, field);
        case FieldType::Float:     return read_field<float>(component, field);
        case FieldType::Double:    return read_field<double>(component, field);
        case FieldType::Bool:      return read_field<bool>(component, field);
        case FieldType::String:    return read_field<std::string>(component, field);
        case FieldType::Vector2f:  return read_field<gryce_engine::math::Vector2f>(component, field);
        case FieldType::Vector3f:  return read_field<gryce_engine::math::Vector3f>(component, field);
        case FieldType::Vector3i:  return read_field<gryce_engine::math::Vector3i>(component, field);
        case FieldType::Vector4f:  return read_field<gryce_engine::math::Vector4f>(component, field);
        case FieldType::Quaternionf:
            return read_field<gryce_engine::math::Quaternionf>(component, field);
        case FieldType::Color:     return read_field<gryce_engine::render::Color>(component, field);
        case FieldType::Enum:      return read_field<int>(component, field);
    }
    return {};
}

const char* translated_field_name(const std::string& name) {
    std::string key = "inspector.field." + name;
    const char* translated = gryce_engine::editor::Localization::instance().get(key.c_str());
    // 找不到翻译时返回原始字段名（避免显示 key）
    if (translated == key.c_str()) return name.c_str();
    return translated;
}

const char* translated_type_name(const std::string& name) {
    std::string key = "inspector.type." + name;
    const char* translated = gryce_engine::editor::Localization::instance().get(key.c_str());
    if (translated == key.c_str()) return name.c_str();
    return translated;
}

} // namespace

namespace gryce_engine::editor {

void InspectorPanel::set_target(scene::Entity* entity) {
    selected_uuid_ = entity ? entity->uuid() : scene::UUID::nil();
}

scene::Entity* InspectorPanel::target_entity() const {
    if (!scene_ || selected_uuid_.str().empty()) return nullptr;
    return scene_->find_entity_by_uuid(selected_uuid_);
}

std::string InspectorPanel::field_key(scene::Entity* entity,
                                      components::Component* component,
                                      const reflection::FieldInfo& field) const {
    if (!entity || !component) return {};
    return std::format("{}:{}:{}", entity->uuid().str(), component->type(), field.name);
}

void InspectorPanel::push_field_command(scene::Entity* entity,
                                        components::Component* component,
                                        const reflection::FieldInfo& field,
                                        const FieldValue& old_value,
                                        const FieldValue& new_value) {
    if (!undo_stack_ || !scene_ || !entity || !component) return;
    undo_stack_->push(std::make_unique<ComponentFieldCommand>(
        *scene_, entity->uuid(), component->type(), field.name,
        old_value, new_value));
}

void InspectorPanel::track_field_change(scene::Entity* entity,
                                        components::Component* component,
                                        const reflection::FieldInfo& field,
                                        const FieldValue& current_value) {
    if (!undo_stack_ || read_only_) return;

    const std::string key = field_key(entity, component, field);
    if (key.empty()) return;

    if (ImGui::IsItemActivated()) {
        active_fields_[key] = current_value;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        auto it = active_fields_.find(key);
        if (it != active_fields_.end()) {
            const FieldValue new_value = read_field_value(component, field);
            if (it->second != new_value) {
                push_field_command(entity, component, field, it->second, new_value);
            }
            active_fields_.erase(it);
        }
    }
}

void InspectorPanel::on_imgui() {
    scene::Entity* entity = target_entity();
    if (!entity) {
        active_fields_.clear();
        ImGui::TextDisabled("%s", tr("inspector.no_entity"));
        return;
    }

    // 切换实体后清理旧实体的编辑中字段
    {
        const std::string prefix = entity->uuid().str() + ":";
        for (auto it = active_fields_.begin(); it != active_fields_.end();) {
            if (it->first.rfind(prefix, 0) != 0) {
                it = active_fields_.erase(it);
            } else {
                ++it;
            }
        }
    }

    if (read_only_) {
        ImGui::BeginDisabled(true);
    }

    // 资源拖放目标：Project 面板拖拽文件到 Inspector
    if (!read_only_ && drop_handler_ && ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload =
                ImGui::AcceptDragDropPayload("GRYCE_PROJECT_FILE", ImGuiDragDropFlags_AcceptBeforeDelivery)) {
            if (payload->IsDelivery() && payload->DataSize > 0) {
                drop_handler_(entity, std::string(static_cast<const char*>(payload->Data)));
            }
        }
        ImGui::EndDragDropTarget();
    }

    // 实体名（可直接编辑）
    char name_buf[128] = {};
    std::strncpy(name_buf, entity->name().c_str(), sizeof(name_buf) - 1);
    if (ImGui::InputText(tr("inspector.name"), name_buf, sizeof(name_buf))) {
        entity->set_name(name_buf);
    }
    if (!read_only_ && undo_stack_ && scene_) {
        const std::string name_key = entity->uuid().str() + ":name";
        if (ImGui::IsItemActivated()) {
            active_fields_[name_key] = entity->name();
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            auto it = active_fields_.find(name_key);
            if (it != active_fields_.end()) {
                const std::string new_name = entity->name();
                const std::string old_name = std::get<std::string>(it->second);
                if (old_name != new_name) {
                    undo_stack_->push(std::make_unique<EntityRenameCommand>(
                        *scene_, entity->uuid(), old_name, new_name));
                }
                active_fields_.erase(it);
            }
        }
    }

    for (const auto& comp : entity->components()) {
        draw_component(entity, comp.get());
    }

    if (read_only_) {
        ImGui::EndDisabled();
    }

    material_editor_.draw();
    animation_editor_.draw();
    terrain_editor_.draw();
    particle_editor_.draw();
}

void InspectorPanel::draw_component(scene::Entity* entity, components::Component* component) {
    if (!entity || !component) return;

    const char* raw_type_name = component->type();
    const char* display_type_name = translated_type_name(raw_type_name);
    ImGui::PushID(static_cast<void*>(component));
    if (ImGui::CollapsingHeader(display_type_name, ImGuiTreeNodeFlags_DefaultOpen)) {
        // MeshRenderer / SkinnedMeshRenderer 额外显示材质编辑入口
        if (std::strcmp(raw_type_name, "MeshRenderer") == 0 ||
            std::strcmp(raw_type_name, "SkinnedMeshRenderer") == 0) {
            draw_material_section(entity, component);
        }

        // SkinnedMeshRenderer 显示动画编辑入口
        if (std::strcmp(raw_type_name, "SkinnedMeshRenderer") == 0) {
            draw_animation_section(entity, component);
        }

        // Terrain 显示地形编辑入口
        if (std::strcmp(raw_type_name, "Terrain") == 0) {
            draw_terrain_section(entity, component);
        }

        // ParticleEmitter2D 显示粒子编辑入口
        if (std::strcmp(raw_type_name, "ParticleEmitter2D") == 0) {
            draw_particle_section(entity, component);
        }

        // PhysicalMaterial 显示物理材质预设选择器
        if (std::strcmp(raw_type_name, "PhysicalMaterial") == 0) {
            draw_physical_material_section(entity, component);
        }

        const auto fields = reflection::Registry::instance().all_fields(raw_type_name);
        if (fields.empty()) {
            ImGui::TextDisabled("%s", tr("inspector.no_fields"));
        }
        for (const reflection::FieldInfo* field : fields) {
            if (field) draw_field(entity, component, *field);
        }
    }
    ImGui::PopID();
}

void InspectorPanel::draw_material_section(scene::Entity* entity, components::Component* component) {
    render::Material* material = nullptr;
    if (auto* mr = dynamic_cast<components::MeshRenderer*>(component)) {
        material = mr->ensure_material();
    } else if (auto* smr = dynamic_cast<components::SkinnedMeshRenderer*>(component)) {
        material = smr->ensure_material();
    }
    if (!material) return;

    ImGui::Separator();
    ImGui::Text("%s: %s", tr("inspector.material"), material->name.c_str());
    ImGui::SameLine();
    if (ImGui::Button(tr("inspector.edit_material"))) {
        material_editor_.open(material, entity);
    }
    ImGui::Separator();
}

void InspectorPanel::draw_animation_section(scene::Entity* entity, components::Component* component) {
    auto* smr = dynamic_cast<components::SkinnedMeshRenderer*>(component);
    if (!smr) return;

    ImGui::Separator();
    ImGui::Text("%s: %s", tr("inspector.clip"),
                smr->clip_name.empty() ? tr("animation_editor.no_clip") : smr->clip_name.c_str());
    ImGui::SameLine();
    if (ImGui::Button(tr("inspector.edit_animation"))) {
        animation_editor_.open(entity);
    }
    ImGui::Separator();
}

void InspectorPanel::draw_terrain_section(scene::Entity* entity, components::Component* component) {
    auto* terrain = dynamic_cast<components::Terrain*>(component);
    if (!terrain) return;

    ImGui::Separator();
    ImGui::Text("%s: %dx%d", tr("inspector.terrain"), terrain->resolution, terrain->resolution);
    ImGui::SameLine();
    if (ImGui::Button(tr("inspector.edit_terrain"))) {
        terrain_editor_.open(entity, terrain);
    }
    ImGui::Separator();
}

void InspectorPanel::draw_particle_section(scene::Entity* entity, components::Component* component) {
    auto* emitter = dynamic_cast<components::d2::ParticleEmitter2D*>(component);
    if (!emitter) return;

    ImGui::Separator();
    ImGui::Text("%s: %d", tr("inspector.particles"), emitter->active_count());
    ImGui::SameLine();
    if (ImGui::Button(tr("inspector.edit_particles"))) {
        particle_editor_.open(entity, emitter);
    }
    ImGui::Separator();
}

void InspectorPanel::draw_physical_material_section(scene::Entity* entity, components::Component* component) {
    using components::PhysicalMaterial;
    auto* pm = dynamic_cast<PhysicalMaterial*>(component);
    if (!pm || !entity) return;

    ImGui::Separator();

    // 构建下拉选项：Custom + 所有内置预设
    const char* custom_label = tr("physical_material.custom");
    std::vector<const char*> items;
    items.reserve(1 + components::k_physical_material_preset_count);
    items.push_back(custom_label);
    int current_index = 0;
    for (int i = 0; i < components::k_physical_material_preset_count; ++i) {
        items.push_back(components::k_physical_material_presets[i].name);
        if (pm->preset_name == components::k_physical_material_presets[i].name) {
            current_index = i + 1;
        }
    }

    int selected = current_index;
    if (ImGui::Combo(tr("physical_material.preset"), &selected, items.data(),
                     static_cast<int>(items.size()))) {
        // 选择新预设前记录旧值快照
        std::vector<ComponentMultiFieldChange> changes;
        changes.push_back({"preset_name", pm->preset_name, pm->preset_name});
        changes.push_back({"softness", pm->softness, pm->softness});
        changes.push_back({"drag_coefficient", pm->drag_coefficient, pm->drag_coefficient});
        changes.push_back({"density", pm->density, pm->density});
        changes.push_back({"friction", pm->friction, pm->friction});

        const char* chosen = items[selected];
        if (chosen == custom_label) {
            pm->preset_name.clear();
        } else {
            pm->apply_preset(chosen);
        }

        // 更新新值并收集真正变化的字段
        for (auto& c : changes) {
            if (c.field_name == "preset_name") c.new_value = pm->preset_name;
            else if (c.field_name == "softness") c.new_value = pm->softness;
            else if (c.field_name == "drag_coefficient") c.new_value = pm->drag_coefficient;
            else if (c.field_name == "density") c.new_value = pm->density;
            else if (c.field_name == "friction") c.new_value = pm->friction;
        }
        changes.erase(std::remove_if(changes.begin(), changes.end(),
                                     [](const ComponentMultiFieldChange& c) {
                                         return c.old_value == c.new_value;
                                     }),
                      changes.end());

        if (undo_stack_ && scene_ && !changes.empty()) {
            undo_stack_->push(std::make_unique<ComponentMultiFieldCommand>(
                *scene_, entity->uuid(), "PhysicalMaterial", std::move(changes)));
        }
    }

    ImGui::Separator();
}

void InspectorPanel::draw_field(scene::Entity* entity,
                                components::Component* component,
                                const reflection::FieldInfo& field) {
    using reflection::FieldType;
    using reflection::read_field;
    using reflection::write_field;

    ImGui::PushID(field.name.c_str());
    if (field.read_only) ImGui::BeginDisabled(true);

    const char* label = translated_field_name(field.name);

    switch (field.type) {
        case FieldType::Bool: {
            bool v = read_field<bool>(component, field);
            if (ImGui::Checkbox(label, &v)) {
                write_field<bool>(component, field, v);
            }
            track_field_change(entity, component, field, v);
            break;
        }
        case FieldType::Int: {
            int v = read_field<int>(component, field);
            if (ImGui::DragInt(label, &v)) {
                write_field<int>(component, field, v);
            }
            track_field_change(entity, component, field, v);
            break;
        }
        case FieldType::Float: {
            float v = read_field<float>(component, field);
            const bool changed = field.has_range
                                     ? ImGui::SliderFloat(label, &v,
                                                          field.range_min, field.range_max)
                                     : ImGui::DragFloat(label, &v, 0.01f);
            if (changed) write_field<float>(component, field, v);
            track_field_change(entity, component, field, v);
            break;
        }
        case FieldType::Double: {
            double dv = read_field<double>(component, field);
            float v = static_cast<float>(dv); // ImGui 无 double 控件，降级为 float
            if (ImGui::DragFloat(label, &v, 0.01f)) {
                write_field<double>(component, field, static_cast<double>(v));
            }
            track_field_change(entity, component, field, static_cast<double>(v));
            break;
        }
        case FieldType::String: {
            std::string v = read_field<std::string>(component, field);
            char buf[256] = {};
            std::strncpy(buf, v.c_str(), sizeof(buf) - 1);
            if (ImGui::InputText(label, buf, sizeof(buf))) {
                write_field<std::string>(component, field, std::string(buf));
            }
            track_field_change(entity, component, field, v);
            break;
        }
        case FieldType::Vector2f: {
            math::Vector2f v = read_field<math::Vector2f>(component, field);
            float arr[2] = {v.x, v.y};
            if (ImGui::DragFloat2(label, arr, 0.01f)) {
                write_field<math::Vector2f>(component, field, math::Vector2f(arr[0], arr[1]));
            }
            track_field_change(entity, component, field, v);
            break;
        }
        case FieldType::Vector3f: {
            math::Vector3f v = read_field<math::Vector3f>(component, field);
            float arr[3] = {v.x, v.y, v.z};
            // 位置字段拖动更灵敏，其它保持默认精度
            const float speed = (field.name == "position") ? 0.1f : 0.01f;
            if (ImGui::DragFloat3(label, arr, speed)) {
                write_field<math::Vector3f>(component, field,
                                            math::Vector3f(arr[0], arr[1], arr[2]));
            }
            track_field_change(entity, component, field, v);
            break;
        }
        case FieldType::Vector4f: {
            math::Vector4f v = read_field<math::Vector4f>(component, field);
            float arr[4] = {v.x, v.y, v.z, v.w};
            if (ImGui::DragFloat4(label, arr, 0.01f)) {
                write_field<math::Vector4f>(component, field,
                                            math::Vector4f(arr[0], arr[1], arr[2], arr[3]));
            }
            track_field_change(entity, component, field, v);
            break;
        }
        case FieldType::Quaternionf: {
            math::Quaternionf q = read_field<math::Quaternionf>(component, field);
            math::Vector3f euler = q.to_euler();
            float arr[3] = {euler.x, euler.y, euler.z};
            if (ImGui::DragFloat3(label, arr, 0.1f)) {
                write_field<math::Quaternionf>(component, field,
                                               math::Quaternionf::from_euler(arr[0], arr[1], arr[2]));
            }
            track_field_change(entity, component, field, q);
            break;
        }
        case FieldType::Vector3i: {
            math::Vector3i v = read_field<math::Vector3i>(component, field);
            int arr[3] = {v.x, v.y, v.z};
            if (ImGui::DragInt3(label, arr)) {
                write_field<math::Vector3i>(component, field, math::Vector3i(arr[0], arr[1], arr[2]));
            }
            track_field_change(entity, component, field, v);
            break;
        }
        case FieldType::Color: {
            render::Color c = read_field<render::Color>(component, field);
            float arr[4] = {c.r, c.g, c.b, c.a};
            if (ImGui::ColorEdit4(label, arr)) {
                write_field<render::Color>(component, field,
                                           render::Color(arr[0], arr[1], arr[2], arr[3]));
            }
            track_field_change(entity, component, field, c);
            break;
        }
        case FieldType::Enum: {
            int v = read_field<int>(component, field);
            std::string enum_key = std::string("inspector.enum.") + component->type() + "." + field.name;
            const char* labels_str = gryce_engine::editor::Localization::instance().get(enum_key.c_str());
            std::vector<std::string> labels;
            if (labels_str != enum_key.c_str()) {
                labels = split_string(labels_str, ';');
            }
            if (!labels.empty() && v >= 0 && v < static_cast<int>(labels.size())) {
                int selected = v;
                std::vector<const char*> items;
                items.reserve(labels.size());
                for (const auto& l : labels) items.push_back(l.c_str());
                if (ImGui::Combo(label, &selected, items.data(), static_cast<int>(items.size()))) {
                    write_field<int>(component, field, selected);
                }
                track_field_change(entity, component, field, v);
            } else {
                if (ImGui::DragInt(label, &v)) {
                    write_field<int>(component, field, v);
                }
                track_field_change(entity, component, field, v);
            }
            break;
        }
    }

    if (field.read_only) ImGui::EndDisabled();
    ImGui::PopID();
}

} // namespace gryce_engine::editor
