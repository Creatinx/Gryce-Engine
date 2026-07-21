#include "hierarchy_panel.h"

#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>

#include "components/component.h"
#include "components/prefab_instance.h"
#include "resources/project.h"
#include "scene/scene.h"
#include "scene/entity.h"
#include "scene/prefab.h"
#include "utils/glog/glog_lib.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "../localization/localization.h"
#include "../undo/commands.h"

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

// Godot 风格节点图标（背景色 + 文字色）
struct NodeIcon {
    const char* label;
    ImU32 bg_color;
    ImU32 text_color;
};

bool type_is_any_of(const char* type, std::initializer_list<const char*> candidates) {
    for (const char* c : candidates) {
        if (std::strcmp(type, c) == 0) return true;
    }
    return false;
}

NodeIcon node_icon_for_entity(const scene::Entity* entity) {
    if (!entity) return {"N", IM_COL32(128, 128, 128, 255), IM_COL32(255, 255, 255, 255)};

    for (const auto& comp : entity->components()) {
        const char* type = comp->type();

        // 摄像机
        if (type_is_any_of(type, {"Camera", "Camera2D"}))
            return {"C", IM_COL32(160, 110, 220, 255), IM_COL32(255, 255, 255, 255)};

        // 光源
        if (type_is_any_of(type, {"Light", "Light2D", "AmbientLight2D"}))
            return {"L", IM_COL32(240, 200, 60, 255), IM_COL32(40, 30, 0, 255)};

        // 音频
        if (type_is_any_of(type, {"AudioSource", "AudioListener"}))
            return {"A", IM_COL32(230, 100, 160, 255), IM_COL32(255, 255, 255, 255)};

        // 网格/模型
        if (type_is_any_of(type, {"MeshRenderer", "SkinnedMeshRenderer"}))
            return {"M", IM_COL32(60, 180, 220, 255), IM_COL32(0, 40, 60, 255)};

        // 刚体
        if (type_is_any_of(type, {"RigidBody", "RigidBody2D"}))
            return {"R", IM_COL32(220, 80, 80, 255), IM_COL32(255, 255, 255, 255)};

        // 静态体
        if (type_is_any_of(type, {"StaticBody", "StaticBody2D"}))
            return {"S", IM_COL32(100, 190, 120, 255), IM_COL32(255, 255, 255, 255)};

        // 碰撞体
        if (type_is_any_of(type, {"BoxCollider", "SphereCollider", "PlaneCollider",
                                  "BoxCollider2D", "CircleCollider2D"}))
            return {"Co", IM_COL32(180, 140, 90, 255), IM_COL32(255, 255, 255, 255)};

        // 角色控制器
        if (type_is_any_of(type, {"CharacterController3D", "CharacterController2D"}))
            return {"CC", IM_COL32(230, 140, 60, 255), IM_COL32(255, 255, 255, 255)};

        // 粒子
        if (type_is_any_of(type, {"ParticleEmitter2D"}))
            return {"P", IM_COL32(220, 220, 220, 255), IM_COL32(40, 40, 40, 255)};

        // UI 控件
        if (type_is_any_of(type, {"BasicRect", "ColorRect", "Label", "Sprite2D",
                                  "Circle", "Polygon", "Tilemap", "ParallaxBackground",
                                  "Skybox2D"}))
            return {"UI", IM_COL32(90, 190, 130, 255), IM_COL32(255, 255, 255, 255)};

        // 2D/3D 节点
        if (std::strcmp(type, "Node2D") == 0)
            return {"2D", IM_COL32(80, 150, 230, 255), IM_COL32(255, 255, 255, 255)};
        if (std::strcmp(type, "Node3D") == 0)
            return {"3D", IM_COL32(230, 140, 70, 255), IM_COL32(255, 255, 255, 255)};

        // 预制体
        if (std::strcmp(type, "PrefabInstance") == 0)
            return {"Pf", IM_COL32(150, 150, 150, 255), IM_COL32(255, 255, 255, 255)};
    }

    return {"N", IM_COL32(128, 128, 128, 255), IM_COL32(255, 255, 255, 255)};
}

// 绘制 Godot 风格树形连线
void draw_tree_lines(int depth, const std::vector<bool>& parent_has_next_sibling,
                     bool is_last_child, float row_screen_x, float row_y,
                     float row_height, float indent, float arrow_offset) {
    if (depth <= 0) return;

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const float y_center = row_y + row_height * 0.5f;
    const float y_bottom = row_y + row_height;
    const ImU32 color = IM_COL32(140, 140, 140, 90);
    const float thickness = 1.0f;

    // 祖先层级竖线：仅当该祖先之后还有兄弟时绘制贯通竖线
    for (int k = 0; k < depth - 1; ++k) {
        if (parent_has_next_sibling[k]) {
            const float x = row_screen_x - (depth - k) * indent + arrow_offset;
            draw_list->AddLine(ImVec2(x, row_y), ImVec2(x, row_y + row_height), color, thickness);
        }
    }

    // 从父节点到当前节点的横线，以及当前节点如果不是最后一个子节点时的向下竖线
    const float parent_x = row_screen_x - indent + arrow_offset;
    const float current_x = row_screen_x + arrow_offset;
    draw_list->AddLine(ImVec2(parent_x, y_center), ImVec2(current_x, y_center), color, thickness);
    if (!is_last_child) {
        draw_list->AddLine(ImVec2(parent_x, y_center), ImVec2(parent_x, y_bottom), color, thickness);
    }
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
    deferred_ops_.push_back(std::move(pending_op_));
    pending_op_ = PendingOp{};
}

void HierarchyPanel::execute_op(const PendingOp& op) {
    if (op.kind == PendingOp::Kind::None || !scene_) return;

    scene::Entity* child = scene_->find_entity_by_uuid(op.child);
    switch (op.kind) {
        case PendingOp::Kind::Delete:
            if (child) {
                if (op.child == selected_uuid_) clear_selection();
                if (undo_stack_) {
                    undo_stack_->push(std::make_unique<EntityDeleteCommand>(*scene_, op.child));
                } else {
                    scene_->destroy_entity(child);
                }
            }
            break;
        case PendingOp::Kind::Reparent:
            if (undo_stack_) {
                undo_stack_->push(std::make_unique<EntityReparentCommand>(*scene_, op.child, op.target));
            } else {
                reparent(child, scene_->find_entity_by_uuid(op.target));
            }
            break;
        case PendingOp::Kind::ReparentToRoot:
            if (undo_stack_) {
                undo_stack_->push(std::make_unique<EntityReparentCommand>(*scene_, op.child,
                                                                          scene::UUID::nil()));
            } else {
                reparent(child, nullptr);
            }
            break;
        case PendingOp::Kind::CreatePrefab: {
            scene::Entity* entity = scene_->find_entity_by_uuid(op.child);
            if (entity) {
                std::filesystem::path root(resources::Project::instance().root());
                std::filesystem::path dir = root / "assets";
                std::error_code ec;
                std::filesystem::create_directories(dir, ec);
                std::string path = (dir / (entity->name() + ".geprefab")).string();
                if (scene::Prefab::save(entity, path)) {
                    auto* inst = entity->add_component<components::PrefabInstance>(path);
                    if (inst) {
                        inst->prefab_path = path;
                        scene::Prefab::refresh_members(entity);
                        GLOG_INFO("Hierarchy: created prefab '{}' and converted entity to instance", path);
                    }
                }
            }
            break;
        }
        case PendingOp::Kind::ApplyPrefab: {
            scene::Entity* entity = scene_->find_entity_by_uuid(op.child);
            auto* inst = entity ? scene::Prefab::get_instance(entity) : nullptr;
            if (inst) {
                if (scene::Prefab::save(entity, inst->prefab_path)) {
                    inst->overrides = nlohmann::json::object();
                    scene::Prefab::clear_cache();
                    GLOG_INFO("Hierarchy: applied prefab changes to '{}'", inst->prefab_path);
                }
            }
            break;
        }
        case PendingOp::Kind::RevertPrefab: {
            scene::Entity* entity = scene_->find_entity_by_uuid(op.child);
            if (entity && scene::Prefab::get_instance(entity)) {
                scene::UUID old_uuid = entity->uuid();
                if (scene::Prefab::revert(entity)) {
                    select(old_uuid);
                    GLOG_INFO("Hierarchy: reverted prefab instance '{}'", entity->name());
                }
            }
            break;
        }
        case PendingOp::Kind::CreateVariant: {
            scene::Entity* entity = scene_->find_entity_by_uuid(op.child);
            auto* inst = entity ? scene::Prefab::get_instance(entity) : nullptr;
            if (inst) {
                std::filesystem::path root(resources::Project::instance().root());
                std::filesystem::path dir = root / "assets" / "prefabs" / "variants";
                std::error_code ec;
                std::filesystem::create_directories(dir, ec);

                std::string variant_path = (dir / (entity->name() + ".geprefabvariant")).string();
                int suffix = 1;
                while (std::filesystem::exists(variant_path, ec)) {
                    variant_path = (dir / (entity->name() + "_" + std::to_string(suffix) + ".geprefabvariant")).string();
                    ++suffix;
                }

                nlohmann::json variant_json;
                variant_json["version"] = 1;
                variant_json["type"] = "prefab_variant";
                variant_json["base_prefab"] = inst->variant_of.empty() ? inst->prefab_path : inst->variant_of;
                variant_json["overrides"] = inst->overrides;

                std::ofstream file(variant_path);
                if (file) {
                    file << variant_json.dump(2);
                    if (file.good()) {
                        inst->prefab_path = variant_path;
                        inst->variant_of = variant_path;
                        scene::Prefab::clear_cache();
                        GLOG_INFO("Hierarchy: created prefab variant '{}'", variant_path);
                    }
                }
            }
            break;
        }
        case PendingOp::Kind::None:
            break;
    }
}

void HierarchyPanel::flush_deferred_ops() {
    if (!scene_) {
        deferred_ops_.clear();
        return;
    }
    for (const auto& op : deferred_ops_) {
        execute_op(op);
    }
    deferred_ops_.clear();
}

bool HierarchyPanel::is_collapsed(const scene::UUID& uuid) const {
    return collapsed_uuids_.find(uuid.str()) != collapsed_uuids_.end();
}

void HierarchyPanel::toggle_collapsed(const scene::UUID& uuid) {
    const std::string key = uuid.str();
    auto it = collapsed_uuids_.find(key);
    if (it != collapsed_uuids_.end()) {
        collapsed_uuids_.erase(it);
    } else {
        collapsed_uuids_.insert(key);
    }
}

void HierarchyPanel::on_imgui() {
    if (!scene_) {
        ImGui::TextDisabled("%s", tr("hierarchy.no_scene"));
        return;
    }

    // Godot 风格紧凑行高
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.0f, 2.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(4.0f, 0.0f));

    // F2 / 慢速双击支持：选中实体时按 F2 进入内联重命名
    if (rename_active_) {
        if (!scene_->find_entity_by_uuid(rename_uuid_)) {
            finish_rename(false);
        }
    } else {
        scene::Entity* selected = selected_entity();
        if (selected && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            ImGui::IsKeyPressed(ImGuiKey_F2)) {
            start_rename(selected);
        }
    }

    const auto& roots = scene_->roots();
    for (size_t i = 0; i < roots.size(); ++i) {
        const bool is_last = (i == roots.size() - 1);
        draw_entity(roots[i].get(), 0, {}, is_last);
    }

    ImGui::PopStyleVar(3);

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
        if (ImGui::MenuItem(tr("hierarchy.create_empty_entity"))) {
            if (undo_stack_) {
                auto cmd = std::make_unique<EntityCreateCommand>(*scene_, tr("hierarchy.new_entity_name"));
                EntityCreateCommand* cmd_ptr = cmd.get();
                undo_stack_->push(std::move(cmd));
                select(cmd_ptr->created_uuid());
            } else {
                scene::Entity* entity = scene_->create_entity(tr("hierarchy.new_entity_name"));
                select(entity->uuid());
            }
        }
        ImGui::EndPopup();
    }

    // 树遍历结束，统一执行本帧记录的删除/换父操作
    execute_pending_op();
}

void HierarchyPanel::draw_entity(scene::Entity* entity, int depth,
                                 const std::vector<bool>& parent_has_next_sibling,
                                 bool is_last_child) {
    if (!entity) return;

    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    const float content_min_x = ImGui::GetWindowContentRegionMin().x;
    const float avail_width = ImGui::GetContentRegionAvail().x;
    const float indent = ImGui::GetStyle().IndentSpacing;
    const float row_height = ImGui::GetFrameHeight();
    const float font_size = ImGui::GetFontSize();
    const float arrow_size = font_size;
    const float icon_size = std::max(12.0f, font_size * 0.78f);
    const float arrow_offset = ImGui::GetStyle().FramePadding.x;
    const float text_offset = arrow_size + ImGui::GetStyle().ItemInnerSpacing.x;

    // 定位到当前深度
    ImGui::SetCursorPosX(content_min_x + depth * indent);
    const ImVec2 row_screen_min = ImGui::GetCursorScreenPos();
    const float row_screen_x = row_screen_min.x;

    // 绘制树形连线
    draw_tree_lines(depth, parent_has_next_sibling, is_last_child,
                    row_screen_x, row_screen_min.y, row_height, indent, arrow_offset);

    ImGui::PushID(static_cast<void*>(entity));

    // 整行可选区（处理选中/悬停背景与点击）
    const bool is_selected = (selected_uuid_.is_valid() && entity->uuid() == selected_uuid_);
    if (ImGui::Selectable("##row", is_selected, ImGuiSelectableFlags_None,
                          ImVec2(avail_width, row_height))) {
        select(entity->uuid());
    }

    // 右键菜单与拖拽绑定到 Selectable
    draw_entity_context_menu(entity);
    handle_drag_drop(entity);

    // 恢复光标到行起点，在 Selectable 之上绘制图标与文本
    ImGui::SetCursorScreenPos(row_screen_min);

    const bool has_children = !entity->children().empty();
    const bool open = !is_collapsed(entity->uuid());

    // 展开/折叠箭头（仅在有子节点时显示）
    if (has_children) {
        const ImVec2 arrow_min = ImGui::GetCursorScreenPos();
        const ImVec2 arrow_max(arrow_min.x + arrow_size, arrow_min.y + row_height);
        ImGui::SetCursorScreenPos(arrow_min);
        ImGui::InvisibleButton("##arrow", ImVec2(arrow_size, row_height));
        if (ImGui::IsItemClicked()) {
            toggle_collapsed(entity->uuid());
        }

        const ImVec2 center((arrow_min.x + arrow_max.x) * 0.5f,
                            (arrow_min.y + arrow_max.y) * 0.5f);
        const float half = arrow_size * 0.18f;
        const ImU32 arrow_col = ImGui::GetColorU32(ImGuiCol_Text);
        if (open) {
            draw_list->AddTriangleFilled(
                ImVec2(center.x - half * 1.8f, center.y - half * 0.8f),
                ImVec2(center.x + half * 1.8f, center.y - half * 0.8f),
                ImVec2(center.x, center.y + half * 1.2f), arrow_col);
        } else {
            draw_list->AddTriangleFilled(
                ImVec2(center.x - half * 0.6f, center.y - half * 1.6f),
                ImVec2(center.x + half * 1.4f, center.y),
                ImVec2(center.x - half * 0.6f, center.y + half * 1.6f), arrow_col);
        }
    }

    // 类型图标
    const NodeIcon icon = node_icon_for_entity(entity);
    const float icon_y = row_screen_min.y + (row_height - icon_size) * 0.5f;
    const ImVec2 icon_min(row_screen_x + text_offset, icon_y);
    const ImVec2 icon_max(icon_min.x + icon_size, icon_min.y + icon_size);
    draw_list->AddRectFilled(icon_min, icon_max, icon.bg_color, 3.0f);

    const ImVec2 icon_text_size = ImGui::CalcTextSize(icon.label);
    const ImVec2 icon_text_pos(icon_min.x + (icon_size - icon_text_size.x) * 0.5f,
                               icon_min.y + (icon_size - icon_text_size.y) * 0.5f);
    draw_list->AddText(icon_text_pos, icon.text_color, icon.label);

    // 名称 / 内联重命名输入框
    const float name_x = row_screen_x + text_offset + icon_size +
                         ImGui::GetStyle().ItemInnerSpacing.x;
    const ImVec2 name_pos(name_x, row_screen_min.y + (row_height - font_size) * 0.5f);
    const bool is_renaming = rename_active_ && entity->uuid() == rename_uuid_;

    if (is_renaming) {
        ImGui::SetCursorScreenPos(name_pos);
        if (rename_first_frame_) {
            ImGui::SetKeyboardFocusHere();
            rename_first_frame_ = false;
        }
        ImGui::PushItemWidth(std::max(20.0f, avail_width - (name_x - row_screen_x)));
        const bool enter_pressed = ImGui::InputText(
            "##rename_inline", rename_buf_, sizeof(rename_buf_),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        const bool deactivated = ImGui::IsItemDeactivated();
        ImGui::PopItemWidth();

        if (enter_pressed) {
            finish_rename(true);
        } else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            finish_rename(false);
        } else if (deactivated) {
            finish_rename(true);
        }
    } else {
        draw_list->AddText(name_pos, ImGui::GetColorU32(ImGuiCol_Text), entity->name().c_str());
    }

    ImGui::PopID();

    // 手动推进光标到下一行起点：Selectable 的光标推进被我们重置过，
    // 后续自定义绘制不会自动推进，必须显式设置，否则所有行堆叠在同一 y 位置。
    ImGui::SetCursorScreenPos(ImVec2(row_screen_min.x, row_screen_min.y + row_height));

    // 递归绘制子实体
    if (has_children && open) {
        auto next_sibling = parent_has_next_sibling;
        next_sibling.push_back(!is_last_child);
        const auto& children = entity->children();
        for (size_t i = 0; i < children.size(); ++i) {
            const bool child_is_last = (i == children.size() - 1);
            draw_entity(children[i].get(), depth + 1, next_sibling, child_is_last);
        }
    }
}

void HierarchyPanel::draw_entity_context_menu(scene::Entity* entity) {
    if (!ImGui::BeginPopupContextItem("##entity_menu")) return;

    if (ImGui::MenuItem(tr("hierarchy.create_empty_child"))) {
        if (undo_stack_) {
            auto cmd = std::make_unique<EntityCreateCommand>(*scene_, tr("hierarchy.new_entity_name"),
                                                             entity->uuid());
            EntityCreateCommand* cmd_ptr = cmd.get();
            undo_stack_->push(std::move(cmd));
            select(cmd_ptr->created_uuid());
        } else {
            auto child = std::make_unique<scene::Entity>(tr("hierarchy.new_entity_name"));
            scene::Entity* added = entity->add_child(std::move(child));
            if (added) select(added->uuid());
        }
    }
    if (ImGui::MenuItem(tr("hierarchy.rename"))) {
        start_rename(entity);
    }
    ImGui::Separator();

    // Prefab 操作
    if (auto* inst = scene::Prefab::get_instance(entity)) {
        if (ImGui::MenuItem(tr("hierarchy.apply_prefab"))) {
            pending_op_.kind = PendingOp::Kind::ApplyPrefab;
            pending_op_.child = entity->uuid();
        }
        if (ImGui::MenuItem(tr("hierarchy.revert_prefab"))) {
            pending_op_.kind = PendingOp::Kind::RevertPrefab;
            pending_op_.child = entity->uuid();
        }
        if (ImGui::MenuItem(tr("hierarchy.create_variant"))) {
            pending_op_.kind = PendingOp::Kind::CreateVariant;
            pending_op_.child = entity->uuid();
        }
        ImGui::Separator();
    } else {
        if (ImGui::MenuItem(tr("hierarchy.create_prefab"))) {
            pending_op_.kind = PendingOp::Kind::CreatePrefab;
            pending_op_.child = entity->uuid();
        }
        ImGui::Separator();
    }

    if (ImGui::MenuItem(tr("hierarchy.delete"))) {
        // 延迟到帧末执行：当前正处于实体树遍历中，直接销毁会使迭代器失效
        pending_op_.kind = PendingOp::Kind::Delete;
        pending_op_.child = entity->uuid();
    }
    ImGui::EndPopup();
}

void HierarchyPanel::start_rename(scene::Entity* entity) {
    if (!entity) return;
    if (rename_active_) {
        finish_rename(true); // 先确认上一个正在编辑的重命名
    }
    rename_uuid_ = entity->uuid();
    std::strncpy(rename_buf_, entity->name().c_str(), sizeof(rename_buf_) - 1);
    rename_buf_[sizeof(rename_buf_) - 1] = '\0';
    rename_active_ = true;
    rename_first_frame_ = true;
}

void HierarchyPanel::finish_rename(bool confirm) {
    if (confirm && scene_) {
        scene::Entity* entity = scene_->find_entity_by_uuid(rename_uuid_);
        if (entity && rename_buf_[0] != '\0') {
            const std::string old_name = entity->name();
            const std::string new_name(rename_buf_);
            if (old_name != new_name) {
                if (undo_stack_) {
                    undo_stack_->push(std::make_unique<EntityRenameCommand>(*scene_, rename_uuid_, old_name, new_name));
                } else {
                    entity->set_name(new_name);
                }
            }
        }
    }
    rename_active_ = false;
    rename_uuid_ = scene::UUID::nil();
    rename_buf_[0] = '\0';
    rename_first_frame_ = false;
}

void HierarchyPanel::draw_inline_rename(scene::Entity* entity, int depth,
                                        const std::vector<bool>& parent_has_next_sibling,
                                        bool is_last_child) {
    // 当前 draw_entity 已经统一处理内联重命名，本函数保留为空实现以保持接口稳定。
    // 如需将来把重命名绘制逻辑拆出，可移回此处。
    (void)entity;
    (void)depth;
    (void)parent_has_next_sibling;
    (void)is_last_child;
}

void HierarchyPanel::handle_drag_drop(scene::Entity* entity) {
    if (drag_enabled_ && ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
        // payload 携带 UUID 字符串而非指针：拖拽跨帧时实体指针可能悬垂，UUID 可安全解析
        const std::string& uuid_str = entity->uuid().str();
        ImGui::SetDragDropPayload("GRYCE_ENTITY", uuid_str.c_str(), uuid_str.size() + 1);
        ImGui::TextUnformatted(std::vformat(tr("hierarchy.move_entity"), std::make_format_args(entity->name())).c_str());
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
