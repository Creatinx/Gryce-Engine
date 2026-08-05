#pragma once

#include <functional>
#include <string>
#include <vector>

#include "scene/uuid.h"

namespace gryce_engine {
namespace scene { class Scene; class Entity; }
} // namespace gryce_engine

namespace gryce_engine::editor {

// ---------------------------------------------------------------------------
// CreateEntityDialog — Godot 风格「创建 Node」模态对话框
// 左侧为收藏/最近使用列表，右侧为搜索 + 分类筛选 + 匹配项列表，
// 底部显示所选类型描述与「创建/取消」按钮。
// 对话框本身不创建实体：通过 set_create_handler 回调交给 HierarchyPanel
// 走既有的 Undo 感知创建辅助函数。
// 收藏与最近使用持久化到 <project_root>/create_entity_dialog.json。
// ---------------------------------------------------------------------------
class CreateEntityDialog {
public:
    // 创建回调：type_id 为注册表中的稳定字符串 id，parent 可为 nullptr（根级创建）
    using CreateHandler = std::function<void(const std::string& type_id, scene::Entity* parent)>;

    void set_create_handler(CreateHandler handler) { create_handler_ = std::move(handler); }

    // 打开对话框；parent 为 nullptr 表示在场景根级创建
    void open(scene::Entity* parent);

    // 每帧调用（在所属面板的 on_imgui 末尾）
    void draw(scene::Scene* scene);

    bool is_open() const { return open_; }

private:
    void load_persistent_state();
    void save_persistent_state() const;
    void push_recent(const std::string& type_id);
    bool is_favorite(const std::string& type_id) const;
    void toggle_favorite(const std::string& type_id);

    void create_selected(scene::Scene* scene, const std::string& type_id);

    CreateHandler create_handler_;

    bool open_ = false;
    bool pending_open_ = false; // open() 只置标志，OpenPopup 在 draw() 中调用（保证 ID 栈一致）
    bool first_frame_ = false;
    scene::UUID parent_uuid_ = scene::UUID::nil();
    char search_[64] = {};
    int filter_category_ = 0; // 0 = 全部
    std::string selected_id_;
    bool persistent_loaded_ = false;

    std::vector<std::string> favorites_;
    std::vector<std::string> recent_; // 最新在前，去重，上限 10
};

} // namespace gryce_engine::editor
