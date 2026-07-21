#pragma once

#include "../editor_panel.h"

#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

#include "../undo/command_stack.h"
#include "scene/uuid.h"

namespace gryce_engine {
namespace scene { class Scene; class Entity; }
} // namespace gryce_engine

namespace gryce_engine::editor {

// ---------------------------------------------------------------------------
// HierarchyPanel — 场景层级面板（M1-E2）
// 实体树（递归 children）、UUID 弱引用选中（实体销毁/场景热重载后自动失效）、
// 右键菜单（创建空实体/创建子实体/重命名/删除）、拖拽换父（可拖回根级）。
// Prefab 实例根显示 [P] 标记。
// 删除/换父在树遍历期间只做记录，帧末统一执行，避免迭代器失效。
// ---------------------------------------------------------------------------
class HierarchyPanel : public EditorPanel {
public:
    HierarchyPanel() : EditorPanel("Hierarchy", "panel.hierarchy") {}

    // 每帧由 EditorApp 设置当前场景（热重载后场景指针会变化）
    void set_scene(scene::Scene* scene);

    // 资源拖放回调：从 Project 面板拖文件到 Hierarchy 时触发。
    // target 为释放目标实体，nullptr 表示拖到空白区域（创建根级实体）。
    void set_drop_handler(std::function<void(scene::Entity*, const std::string&)> handler) {
        drop_handler_ = std::move(handler);
    }

    // UUID 弱引用选中：访问时实时解析，实体已销毁则自动清除
    scene::Entity* selected_entity();
    void select(const scene::UUID& id) { selected_uuid_ = id; }
    void clear_selection() { selected_uuid_ = scene::UUID::nil(); }

    // Play Mode 下禁止实体树拖拽换父与 Project 资源拖入创建实体
    void set_drag_enabled(bool enabled) { drag_enabled_ = enabled; }

    // 绑定 Undo/Redo 命令栈
    void set_undo_stack(CommandStack* stack) { undo_stack_ = stack; }

    // 将本帧记录的延迟操作（删除/换父/Prefab 等）统一执行。
    // 必须在当前帧所有渲染命令已提交并呈现后调用（render_ctx.present 之后），
    // 避免渲染线程仍引用已被销毁的实体/材质资源。
    void flush_deferred_ops();

protected:
    void on_imgui() override;

private:
    // 帧末执行的延迟操作（树遍历期间不允许改动场景结构）
    struct PendingOp {
        enum class Kind { None, Delete, Reparent, ReparentToRoot, CreatePrefab, ApplyPrefab, RevertPrefab, CreateVariant };
        Kind kind = Kind::None;
        scene::UUID child;
        scene::UUID target; // 仅 Reparent 使用
        std::string path;   // CreatePrefab 使用
    };

    void draw_entity(scene::Entity* entity, int depth = 0,
                     const std::vector<bool>& parent_has_next_sibling = {},
                     bool is_last_child = true);
    void draw_entity_context_menu(scene::Entity* entity);
    void handle_drag_drop(scene::Entity* entity);
    void execute_pending_op();
    void execute_op(const PendingOp& op);
    void reparent(scene::Entity* child, scene::Entity* new_parent); // new_parent=nullptr 表示移到根级
    bool is_ancestor_of(scene::Entity* maybe_ancestor, scene::Entity* entity) const;

    scene::Scene* scene_ = nullptr;
    std::function<void(scene::Entity*, const std::string&)> drop_handler_;
    scene::UUID selected_uuid_ = scene::UUID::nil();
    bool drag_enabled_ = true;
    CommandStack* undo_stack_ = nullptr;

    PendingOp pending_op_;
    std::vector<PendingOp> deferred_ops_;

    // Godot 风格树：手动维护折叠状态（TreeNodeEx 不再管理展开/折叠）
    std::unordered_set<std::string, std::hash<std::string>> collapsed_uuids_;
    bool is_collapsed(const scene::UUID& uuid) const;
    void toggle_collapsed(const scene::UUID& uuid);

    // 内联重命名状态
    scene::UUID rename_uuid_ = scene::UUID::nil();
    char rename_buf_[128] = {};
    bool rename_active_ = false;
    bool rename_first_frame_ = false;

    void start_rename(scene::Entity* entity);
    void finish_rename(bool confirm);
    void draw_inline_rename(scene::Entity* entity, int depth,
                            const std::vector<bool>& parent_has_next_sibling,
                            bool is_last_child);
};

} // namespace gryce_engine::editor
