#pragma once

#include "../editor_panel.h"

#include <functional>

namespace gryce_engine {
namespace scene { class Entity; }
namespace components { class Component; }
namespace reflection { struct FieldInfo; }
} // namespace gryce_engine

namespace gryce_engine::editor {

// ---------------------------------------------------------------------------
// InspectorPanel — 属性检查面板（M1-E1）
// 显示选中实体的名称与全部组件；组件字段通过反射系统
// （core/reflection）按 FieldType 分派编辑控件，可读写。
// ---------------------------------------------------------------------------
class InspectorPanel : public EditorPanel {
public:
    InspectorPanel() : EditorPanel("Inspector") {}

    // 每帧由 EditorApp 设置当前选中实体（Hierarchy 面板的选择结果）
    void set_target(scene::Entity* entity) { entity_ = entity; }

    // Play Mode 下只读展示，禁止修改字段与资源拖放
    void set_read_only(bool read_only) { read_only_ = read_only; }

    // 资源拖放回调：从 Project 面板拖文件到 Inspector 时触发（参数为 res:/ 路径）
    void set_drop_handler(std::function<void(scene::Entity*, const std::string&)> handler) {
        drop_handler_ = std::move(handler);
    }

protected:
    void on_imgui() override;

private:
    void draw_component(components::Component* component);
    void draw_field(components::Component* component, const reflection::FieldInfo& field);

    scene::Entity* entity_ = nullptr;
    std::function<void(scene::Entity*, const std::string&)> drop_handler_;
    bool read_only_ = false;
};

} // namespace gryce_engine::editor
