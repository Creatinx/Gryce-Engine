#include "inspector_panel.h"

#include <cstring>

#include "components/component.h"
#include "reflection/reflection.h"
#include "scene/entity.h"
#include "../localization/localization.h"

namespace gryce_engine::editor {

void InspectorPanel::on_imgui() {
    if (!entity_) {
        ImGui::TextDisabled("%s", tr("inspector.no_entity"));
        return;
    }

    if (read_only_) {
        ImGui::BeginDisabled(true);
    }

    // 资源拖放目标：Project 面板拖拽文件到 Inspector
    if (!read_only_ && drop_handler_ && ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload =
                ImGui::AcceptDragDropPayload("GRYCE_PROJECT_FILE", ImGuiDragDropFlags_AcceptBeforeDelivery)) {
            if (payload->IsDelivery() && payload->DataSize > 0) {
                drop_handler_(entity_, std::string(static_cast<const char*>(payload->Data)));
            }
        }
        ImGui::EndDragDropTarget();
    }

    // 实体名（可直接编辑）
    char name_buf[128] = {};
    std::strncpy(name_buf, entity_->name().c_str(), sizeof(name_buf) - 1);
    if (ImGui::InputText(tr("inspector.name"), name_buf, sizeof(name_buf))) {
        entity_->set_name(name_buf);
    }

    for (const auto& comp : entity_->components()) {
        draw_component(comp.get());
    }

    if (read_only_) {
        ImGui::EndDisabled();
    }
}

void InspectorPanel::draw_component(components::Component* component) {
    if (!component) return;

    const char* type_name = component->type();
    ImGui::PushID(static_cast<void*>(component));
    if (ImGui::CollapsingHeader(type_name, ImGuiTreeNodeFlags_DefaultOpen)) {
        const auto fields = reflection::Registry::instance().all_fields(type_name);
        if (fields.empty()) {
            ImGui::TextDisabled("%s", tr("inspector.no_fields"));
        }
        for (const reflection::FieldInfo* field : fields) {
            if (field) draw_field(component, *field);
        }
    }
    ImGui::PopID();
}

void InspectorPanel::draw_field(components::Component* component,
                                const reflection::FieldInfo& field) {
    using reflection::FieldType;
    using reflection::read_field;
    using reflection::write_field;

    ImGui::PushID(field.name.c_str());
    if (field.read_only) ImGui::BeginDisabled(true);

    switch (field.type) {
        case FieldType::Bool: {
            bool v = read_field<bool>(component, field);
            if (ImGui::Checkbox(field.display_name.c_str(), &v)) {
                write_field<bool>(component, field, v);
            }
            break;
        }
        case FieldType::Int: {
            int v = read_field<int>(component, field);
            if (ImGui::DragInt(field.display_name.c_str(), &v)) {
                write_field<int>(component, field, v);
            }
            break;
        }
        case FieldType::Float: {
            float v = read_field<float>(component, field);
            const bool changed = field.has_range
                                     ? ImGui::SliderFloat(field.display_name.c_str(), &v,
                                                          field.range_min, field.range_max)
                                     : ImGui::DragFloat(field.display_name.c_str(), &v, 0.01f);
            if (changed) write_field<float>(component, field, v);
            break;
        }
        case FieldType::Double: {
            double dv = read_field<double>(component, field);
            float v = static_cast<float>(dv); // ImGui 无 double 控件，降级为 float
            if (ImGui::DragFloat(field.display_name.c_str(), &v, 0.01f)) {
                write_field<double>(component, field, static_cast<double>(v));
            }
            break;
        }
        case FieldType::String: {
            std::string v = read_field<std::string>(component, field);
            char buf[256] = {};
            std::strncpy(buf, v.c_str(), sizeof(buf) - 1);
            if (ImGui::InputText(field.display_name.c_str(), buf, sizeof(buf))) {
                write_field<std::string>(component, field, std::string(buf));
            }
            break;
        }
        case FieldType::Vector2f: {
            math::Vector2f v = read_field<math::Vector2f>(component, field);
            float arr[2] = {v.x, v.y};
            if (ImGui::DragFloat2(field.display_name.c_str(), arr, 0.01f)) {
                write_field<math::Vector2f>(component, field, math::Vector2f(arr[0], arr[1]));
            }
            break;
        }
        case FieldType::Vector3f: {
            math::Vector3f v = read_field<math::Vector3f>(component, field);
            float arr[3] = {v.x, v.y, v.z};
            if (ImGui::DragFloat3(field.display_name.c_str(), arr, 0.01f)) {
                write_field<math::Vector3f>(component, field,
                                            math::Vector3f(arr[0], arr[1], arr[2]));
            }
            break;
        }
        case FieldType::Vector4f: {
            math::Vector4f v = read_field<math::Vector4f>(component, field);
            float arr[4] = {v.x, v.y, v.z, v.w};
            if (ImGui::DragFloat4(field.display_name.c_str(), arr, 0.01f)) {
                write_field<math::Vector4f>(component, field,
                                            math::Vector4f(arr[0], arr[1], arr[2], arr[3]));
            }
            break;
        }
        case FieldType::Quaternionf: {
            // 四元数直接编辑易出错，本轮只读展示（后续换欧拉角 + gizmo）
            math::Quaternionf q = read_field<math::Quaternionf>(component, field);
            ImGui::Text("%s: (%.3f, %.3f, %.3f, %.3f)", field.display_name.c_str(),
                        static_cast<double>(q.x), static_cast<double>(q.y),
                        static_cast<double>(q.z), static_cast<double>(q.w));
            break;
        }
    }

    if (field.read_only) ImGui::EndDisabled();
    ImGui::PopID();
}

} // namespace gryce_engine::editor
