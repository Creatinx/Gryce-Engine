#include "hierarchy_panel.h"

#include <cstring>

#include "components/prefab_instance.h"
#include "scene/scene.h"
#include "scene/entity.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::editor {

namespace {

// 从当前持有者（父实体或场景根列表）摘下实体所有权。
// 供拖拽换父使用；返回 nullptr 表示未找到。
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

} // namespace

void HierarchyPanel::set_scene(scene::Scene* scene) {
    if (scene_ != scene) {
        // 场景热重载后旧实体全部失效，选中随 UUID 解析失败自然失效
        scene_ = scene;
    }
}

scene::Entity* HierarchyPanel::selected_entity() {
    if (!scene_ || !selected_uuid_.is_valid()) return nullptr;
    scene::Entity* entity = scene_->find_entity_by_uuid(selected_uuid_);
    if (!entity) {
        // 实体已被删除：弱引用自动失效
        selected_uuid_ = scene::UUID::nil();
    }
    return entity;
}

bool HierarchyPanel::is_ancestor_of(scene::Entity* maybe_ancestor, scene::Entity* entity) const {
    for (scene::Entity* p = entity ? entity->parent() : nullptr; p; p = p->parent()) {
        if (p == maybe_ancestor) return true;
    }
    return false;
}

void HierarchyPanel::reparent(scene::Entity* child, scene::Entity* new_parent) {
    if (!scene_ || !child || child == new_parent) return;
    // 禁止环：不能把实体拖到自己的后代下
    if (new_parent && is_ancestor_of(child, new_parent)) {
        GLOG_WARN("Hierarchy: cannot reparent '{}' to its own descendant", child->name());
        return;
    }
    if (child->parent() == new_parent) return; // 无变化

    std::unique_ptr<scene::Entity> owned = detach_entity(*scene_, child);
    if (!owned) return;
    if (new_parent) {
        new_parent->add_child(std::move(owned));
    } else {
        scene_->add_root_entity(std::move(owned));
    }
    GLOG_INFO("Hierarchy: reparented '{}' to '{}'", child->name(),
              new_parent ? new_parent->name() : "<root>");
}

void HierarchyPanel::execute_pending_op() {
    if (pending_op_.kind == PendingOp::Kind::None || !scene_) return;

    scene::Entity* child = scene_->find_entity_by_uuid(pending_op_.child);
    switch (pending_op_.kind) {
        case PendingOp::Kind::Delete:
            if (child) {
                if (pending_op_.child == selected_uuid_) clear_selection();
                // destroy_entity 递归销毁子实体并反注册组件
                scene_->destroy_entity(child);
            }
            break;
        case PendingOp::Kind::Reparent:
            reparent(child, scene_->find_entity_by_uuid(pending_op_.target));
            break;
        case PendingOp::Kind::ReparentToRoot:
            reparent(child, nullptr);
            break;
        case PendingOp::Kind::None:
            break;
    }
    pending_op_ = PendingOp{};
}

void HierarchyPanel::on_imgui() {
    if (!scene_) {
        ImGui::TextDisabled("No scene loaded");
        return;
    }

    for (const auto& root : scene_->roots()) {
        draw_entity(root.get());
    }

    // 空白区域填充为 Dummy：左击取消选中、接收拖到根级、右击创建菜单
    ImGui::Dummy(ImGui::GetContentRegionAvail());
    if (ImGui::BeginDragDropTarget()) {
        if (drag_enabled_) {
            if (const ImGuiPayload* payload =
                    ImGui::AcceptDragDropPayload("GRYCE_ENTITY", ImGuiDragDropFlags_AcceptBeforeDelivery)) {
                if (payload->IsDelivery()) {
                    pending_op_.kind = PendingOp::Kind::ReparentToRoot;
                    pending_op_.child = scene::UUID(static_cast<const char*>(payload->Data));
                }
            }
            if (drop_handler_) {
                if (const ImGuiPayload* payload =
                        ImGui::AcceptDragDropPayload("GRYCE_PROJECT_FILE", ImGuiDragDropFlags_AcceptBeforeDelivery)) {
                    if (payload->IsDelivery() && payload->DataSize > 0) {
                        drop_handler_(nullptr, std::string(static_cast<const char*>(payload->Data)));
                    }
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        clear_selection();
    }
    if (ImGui::BeginPopupContextItem("##hierarchy_bg_menu")) {
        if (ImGui::MenuItem("Create Empty Entity")) {
            scene::Entity* entity = scene_->create_entity("New Entity");
            select(entity->uuid());
        }
        ImGui::EndPopup();
    }

    draw_rename_popup();

    // 树遍历结束，统一执行本帧记录的删除/换父操作
    execute_pending_op();
}

void HierarchyPanel::draw_entity(scene::Entity* entity) {
    if (!entity) return;

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                               ImGuiTreeNodeFlags_SpanAvailWidth |
                               ImGuiTreeNodeFlags_DefaultOpen;
    if (entity->children().empty()) {
        flags |= ImGuiTreeNodeFlags_Leaf;
    }
    if (selected_uuid_.is_valid() && entity->uuid() == selected_uuid_) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }

    // Prefab 实例根显示 [P] 标记
    const bool is_prefab = entity->get_component<components::PrefabInstance>() != nullptr;
    char label[160] = {};
    std::snprintf(label, sizeof(label), "%s%s", is_prefab ? "[P] " : "", entity->name().c_str());

    const bool open = ImGui::TreeNodeEx(static_cast<void*>(entity), flags, "%s", label);
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        select(entity->uuid());
    }

    draw_entity_context_menu(entity);
    handle_drag_drop(entity);

    if (open) {
        for (const auto& child : entity->children()) {
            draw_entity(child.get());
        }
        ImGui::TreePop();
    }
}

void HierarchyPanel::draw_entity_context_menu(scene::Entity* entity) {
    if (!ImGui::BeginPopupContextItem("##entity_menu")) return;

    if (ImGui::MenuItem("Create Empty Child")) {
        auto child = std::make_unique<scene::Entity>("New Entity");
        scene::Entity* added = entity->add_child(std::move(child));
        if (added) select(added->uuid());
    }
    if (ImGui::MenuItem("Rename...")) {
        rename_uuid_ = entity->uuid();
        std::strncpy(rename_buf_, entity->name().c_str(), sizeof(rename_buf_) - 1);
        rename_buf_[sizeof(rename_buf_) - 1] = '\0';
        rename_open_requested_ = true;
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Delete")) {
        // 延迟到帧末执行：当前正处于实体树遍历中，直接销毁会使迭代器失效
        pending_op_.kind = PendingOp::Kind::Delete;
        pending_op_.child = entity->uuid();
    }
    ImGui::EndPopup();
}

void HierarchyPanel::draw_rename_popup() {
    if (rename_open_requested_) {
        ImGui::OpenPopup("Rename Entity");
        rename_open_requested_ = false;
    }
    if (ImGui::BeginPopupModal("Rename Entity", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        scene::Entity* entity = scene_ ? scene_->find_entity_by_uuid(rename_uuid_) : nullptr;
        if (!entity) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }
        ImGui::Text("Rename '%s'", entity->name().c_str());
        const bool confirmed = ImGui::InputText("##rename_input", rename_buf_, sizeof(rename_buf_),
                                                ImGuiInputTextFlags_EnterReturnsTrue);
        if (confirmed || ImGui::Button("OK")) {
            if (rename_buf_[0] != '\0') {
                entity->set_name(rename_buf_);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void HierarchyPanel::handle_drag_drop(scene::Entity* entity) {
    if (drag_enabled_ && ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
        // payload 携带 UUID 字符串而非指针：拖拽跨帧时实体指针可能悬垂，UUID 可安全解析
        const std::string& uuid_str = entity->uuid().str();
        ImGui::SetDragDropPayload("GRYCE_ENTITY", uuid_str.c_str(), uuid_str.size() + 1);
        ImGui::Text("Move '%s'", entity->name().c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (drag_enabled_) {
            if (const ImGuiPayload* payload =
                    ImGui::AcceptDragDropPayload("GRYCE_ENTITY", ImGuiDragDropFlags_AcceptBeforeDelivery)) {
                if (payload->IsDelivery()) {
                    // 延迟到帧末执行（同 Delete）
                    pending_op_.kind = PendingOp::Kind::Reparent;
                    pending_op_.child = scene::UUID(static_cast<const char*>(payload->Data));
                    pending_op_.target = entity->uuid();
                }
            }
            if (drop_handler_) {
                if (const ImGuiPayload* payload =
                        ImGui::AcceptDragDropPayload("GRYCE_PROJECT_FILE", ImGuiDragDropFlags_AcceptBeforeDelivery)) {
                    if (payload->IsDelivery() && payload->DataSize > 0) {
                        drop_handler_(entity, std::string(static_cast<const char*>(payload->Data)));
                    }
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
}

} // namespace gryce_engine::editor
