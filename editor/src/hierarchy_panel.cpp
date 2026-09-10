#include "hierarchy_panel.h"

#include <cstdint>
#include <string>

#include <imgui.h>

#include "command_stack.h"
#include "i18n.h"
#include "fluent_components.h"

namespace gryce_engine::editor {

namespace {
const char* tr(const char* key) { return I18n::instance().tr(key); }
} // namespace

scene::Entity* HierarchyPanel::selected_entity() const {
    if (!scene_ || !selected_uuid_.is_valid()) return nullptr;
    return scene_->find_entity_by_uuid(selected_uuid_);
}

void HierarchyPanel::render() {
    ImGui::Begin("Scene");
    if (!scene_) {
        ImGui::TextUnformatted(tr("No active scene"));
        ImGui::End();
        return;
    }

    FluentButton create_btn(/*accent=*/true);
    if (create_btn.Draw(tr("Create Entity"), fluent_theme())) {
        create_entity(selected_entity());
    }

    ImGui::Separator();

    ImGui::BeginChild("hierarchy_tree");
    const auto& roots = scene_->root()->children();
    for (const auto& child : roots) {
        render_entity(child.get(), 0);
    }
    ImGui::EndChild();

    // ImGui 上下文菜单处理
    scene_->foreach([this](scene::Entity* entity) {
        if (entity == scene_->root()) return;
        const std::string popup_id = "entity_menu_" + entity->uuid().str();
        if (ImGui::BeginPopup(popup_id.c_str())) {
            if (ImGui::MenuItem(tr("Create Child"))) {
                create_entity(entity);
            }
            if (ImGui::MenuItem(tr("Delete"))) {
                queue_delete(entity);
            }
            ImGui::EndPopup();
        }
    });

    ImGui::End();
}

void HierarchyPanel::render_entity(scene::Entity* entity, int depth) {
    if (!entity) return;

    const bool selected = entity->uuid() == selected_uuid_;
    const ImGuiTreeNodeFlags flags =
        ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
        ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen |
        (entity->children().empty() ? ImGuiTreeNodeFlags_Leaf : 0) |
        (selected ? ImGuiTreeNodeFlags_Selected : 0);

    const std::string label = entity->name().empty() ? "Entity" : entity->name();
    const std::string node_id = "##entity_" + entity->uuid().str();

    const bool open = ImGui::TreeNodeEx(node_id.c_str(), flags, "%s", label.c_str());
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) ||
        ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        selected_uuid_ = entity->uuid();
        if (on_selection_changed_) on_selection_changed_(entity);
    }

    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        ImGui::OpenPopup(("entity_menu_" + entity->uuid().str()).c_str());
    }

    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
        scene::Entity* payload = entity;
        ImGui::SetDragDropPayload("GRYCE_ENTITY", &payload, sizeof(payload));
        ImGui::TextUnformatted(label.c_str());
        ImGui::EndDragDropSource();
    }

    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload =
                ImGui::AcceptDragDropPayload("GRYCE_ENTITY")) {
            scene::Entity* source = *static_cast<scene::Entity* const*>(payload->Data);
            if (source && source != entity && !is_descendant_of(source, entity)) {
                reparent_entity(source, entity);
            }
        }
        ImGui::EndDragDropTarget();
    }

    if (open) {
        for (const auto& child : entity->children()) {
            render_entity(child.get(), depth + 1);
        }
        ImGui::TreePop();
    }
}

void HierarchyPanel::create_entity(scene::Entity* parent) {
    if (!scene_) return;

    if (command_stack_) {
        auto* command = command_stack_->push(
            std::make_unique<CreateEntityCommand>(scene_, "Entity", parent));
        auto* create_cmd = dynamic_cast<CreateEntityCommand*>(command);
        scene::Entity* entity =
            create_cmd ? create_cmd->created_entity() : nullptr;
        if (entity) {
            selected_uuid_ = entity->uuid();
            if (on_selection_changed_) on_selection_changed_(entity);
        }
        return;
    }

    scene::Entity* entity = scene_->create_entity("Entity");
    if (parent && parent != entity) {
        auto owned = scene_->root()->detach_child(entity);
        if (owned) {
            parent->add_child(std::move(owned));
        }
    }
    selected_uuid_ = entity->uuid();
    if (on_selection_changed_) on_selection_changed_(entity);
}

void HierarchyPanel::queue_delete(scene::Entity* entity) {
    if (!entity) return;
    for (auto* existing : queued_deletes_) {
        if (existing == entity) return;
    }
    queued_deletes_.push_back(entity);
}

void HierarchyPanel::reparent_entity(scene::Entity* entity, scene::Entity* new_parent) {
    if (!scene_ || !entity || !new_parent || entity == new_parent) return;
    if (is_descendant_of(entity, new_parent)) return;

    if (command_stack_) {
        command_stack_->push(
            std::make_unique<ReparentEntityCommand>(scene_, entity, new_parent));
        return;
    }

    std::unique_ptr<scene::Entity> owned;
    if (entity->parent()) {
        owned = entity->parent()->detach_child(entity);
    } else {
        owned = scene_->root()->detach_child(entity);
    }
    if (!owned) return;

    new_parent->add_child(std::move(owned));
}

bool HierarchyPanel::is_descendant_of(scene::Entity* entity, scene::Entity* ancestor) const {
    if (!entity || !ancestor) return false;
    scene::Entity* cursor = entity;
    while (cursor) {
        if (cursor == ancestor) return true;
        cursor = cursor->parent();
    }
    return false;
}

} // namespace gryce_engine::editor
