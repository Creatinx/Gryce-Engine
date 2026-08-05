#pragma once

#include "../editor_panel.h"

#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

#include "../ui/create_entity_dialog.h"

#include "../undo/command_stack.h"
#include "components/light.h"
#include "scene/entity.h"
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
    HierarchyPanel() : EditorPanel("Hierarchy", "panel.hierarchy") {
        // 创建实体对话框：创建动作统一走本面板的 Undo 感知辅助函数
        create_entity_dialog_.set_create_handler(
            [this](const std::string& type_id, scene::Entity* parent) {
                create_entity_of_type(type_id, parent);
            });
    }

    // 每帧由 EditorApp 设置当前场景（热重载后场景指针会变化）
    void set_scene(scene::Scene* scene);

    // 资源拖放回调：从 Project 面板拖文件到 Hierarchy 时触发。
    // target 为释放目标实体，nullptr 表示拖到空白区域（创建根级实体）。
    void set_drop_handler(std::function<void(scene::Entity*, const std::string&)> handler) {
        drop_handler_ = std::move(handler);
    }

    // 聚焦回调：Hierarchy 右键 "Frame Selected" / 双击空白处时触发，
    // 由 EditorApp 将编辑器相机对准选中实体包围盒。
    void set_focus_handler(std::function<void(scene::Entity*)> handler) {
        focus_handler_ = std::move(handler);
    }

    // UUID 弱引用选中：访问时实时解析，实体已销毁则自动清除
    scene::Entity* selected_entity();
    void select(const scene::UUID& id) { selected_uuid_ = id; }
    void clear_selection() { selected_uuid_ = scene::UUID::nil(); }

    // Play Mode 下禁止实体树拖拽换父与 Project 资源拖入创建实体
    void set_drag_enabled(bool enabled) { drag_enabled_ = enabled; }

    // 绑定 Undo/Redo 命令栈
    void set_undo_stack(CommandStack* stack) { undo_stack_ = stack; }

    // 组件增删后触发外部系统（如 PhysicsSystem3D）即时热重载
    void set_component_changed_handler(std::function<void(scene::Entity*)> handler) {
        component_changed_handler_ = std::move(handler);
    }

    // 将指定实体排队到本帧延迟删除队列，实际删除在 render_ctx.present 之后执行。
    void queue_delete(const scene::UUID& uuid);

    // 剪贴板 / 副本操作（右键菜单与全局快捷键共用）
    void cut_entity(scene::Entity* entity);
    void copy_entity(scene::Entity* entity);
    void paste_clipboard(scene::Entity* parent); // parent=nullptr 粘贴到根级
    void duplicate_entity(scene::Entity* entity);
    void rename_selected();
    bool has_clipboard() const { return clipboard_ != nullptr; }

    // 打开「添加组件」选择器（目标为任意实体，供 Inspector 的 Add Component 按钮复用）。
    // 实际 OpenPopup 延迟到 draw_component_picker() 内执行，保证与调用方窗口 ID 栈无关。
    void open_component_picker(scene::Entity* target_entity);

    // 将本帧记录的延迟操作（删除/换父/Prefab 等）统一执行。
    // 必须在当前帧所有渲染命令已提交并呈现后调用（render_ctx.present 之后），
    // 避免渲染线程仍引用已被销毁的实体/材质资源。
    void flush_deferred_ops();

protected:
    void on_imgui() override;

private:
    // 帧末执行的延迟操作（树遍历期间不允许改动场景结构）
    struct PendingOp {
        enum class Kind { None, Delete, Reparent, ReparentToRoot, CreatePrefab, ApplyPrefab, RevertPrefab, CreateVariant, Duplicate, Paste };
        Kind kind = Kind::None;
        scene::UUID child;
        scene::UUID target; // Reparent / Paste（父级，nil = 根级）使用
        std::string path;   // CreatePrefab 使用
    };

    void draw_entity(scene::Entity* entity, int depth = 0,
                     const std::vector<bool>& parent_has_next_sibling = {},
                     bool is_last_child = true);
    void draw_entity_context_menu(scene::Entity* entity);
    // 场景根节点行的右键菜单（仅 新建 / 粘贴到根级）
    void draw_scene_root_context_menu();

    // 创建实体辅助函数（统一处理 Undo/Redo 与非 Undo 路径）
    scene::Entity* create_empty_entity(const char* name, scene::Entity* parent_entity);
    scene::Entity* create_node3d(const char* name, scene::Entity* parent_entity);
    scene::Entity* create_node2d(const char* name, scene::Entity* parent_entity);
    scene::Entity* create_mesh_entity(const char* name, scene::Entity* parent_entity, const char* mesh_path);
    scene::Entity* create_camera(scene::Entity* parent_entity);
    scene::Entity* create_light(components::Light::Type type, scene::Entity* parent_entity);
    // 创建实体对话框回调入口：把注册表 type_id 映射到上面的辅助函数
    void create_entity_of_type(const std::string& type_id, scene::Entity* parent_entity);
    void draw_component_picker();
    void handle_drag_drop(scene::Entity* entity);
    void execute_pending_op();
    void execute_op(const PendingOp& op);
    void reparent(scene::Entity* child, scene::Entity* new_parent); // new_parent=nullptr 表示移到根级
    bool is_ancestor_of(scene::Entity* maybe_ancestor, scene::Entity* entity) const;

    scene::Scene* scene_ = nullptr;
    std::function<void(scene::Entity*, const std::string&)> drop_handler_;
    std::function<void(scene::Entity*)> focus_handler_;
    scene::UUID selected_uuid_ = scene::UUID::nil();
    bool drag_enabled_ = true;
    CommandStack* undo_stack_ = nullptr;
    std::function<void(scene::Entity*)> component_changed_handler_;

    PendingOp pending_op_;
    std::vector<PendingOp> deferred_ops_;

    // 实体剪贴板（Cut/Copy/Paste）；clone 产生新 UUID，可直接粘贴
    std::unique_ptr<scene::Entity> clipboard_;

    // Godot 风格树：手动维护折叠状态（TreeNodeEx 不再管理展开/折叠）
    std::unordered_set<std::string, std::hash<std::string>> collapsed_uuids_;
    bool is_collapsed(const scene::UUID& uuid) const;
    void toggle_collapsed(const scene::UUID& uuid);

    // 内联重命名状态
    scene::UUID rename_uuid_ = scene::UUID::nil();
    char rename_buf_[128] = {};
    bool rename_active_ = false;
    bool rename_first_frame_ = false;

    // 组件选择器状态
    bool component_picker_open_ = false;
    bool component_picker_first_frame_ = false;
    bool component_picker_pending_open_ = false; // OpenPopup 延迟到 draw()（与 CreateEntityDialog 同理）
    scene::UUID component_picker_target_uuid_ = scene::UUID::nil();
    char component_picker_search_[64] = {};

    // Godot 风格「创建 Node」对话框
    CreateEntityDialog create_entity_dialog_;

    void start_rename(scene::Entity* entity);
    void finish_rename(bool confirm);
    void draw_inline_rename(scene::Entity* entity, int depth,
                            const std::vector<bool>& parent_has_next_sibling,
                            bool is_last_child);
};

} // namespace gryce_engine::editor
