#pragma once

#include "../editor_panel.h"

#include <functional>
#include <string>
#include <unordered_map>

#include "scene/uuid.h"

#include "../ui/animation_editor_window.h"
#include "../ui/material_editor_window.h"
#include "../ui/terrain_editor_window.h"
#include "../ui/particle_editor_window.h"
#include "../undo/command_stack.h"
#include "../undo/field_value.h"

namespace gryce_engine {
namespace scene { class Entity; class Scene; class UUID; }
namespace components { class Component; }
namespace reflection { struct FieldInfo; }
} // namespace gryce_engine

namespace gryce_engine::editor {

// ---------------------------------------------------------------------------
// InspectorPanel — 属性检查面板（M1-E1）
// 显示选中实体的名称与全部组件；组件字段通过反射系统
// （core/reflection）按 FieldType 分派编辑控件，可读写。
// 字段编辑自动产生 Undo/Redo 命令。
// ---------------------------------------------------------------------------
class InspectorPanel : public EditorPanel {
public:
    InspectorPanel() : EditorPanel("Inspector", "panel.inspector") {}

    // 每帧由 EditorApp 设置当前选中实体（Hierarchy 面板的选择结果）。
    // 内部保存 UUID 弱引用，避免实体被删除后 Inspector 仍访问悬垂指针。
    void set_target(scene::Entity* entity);

    // 设置当前场景（Undo 命令需要）
    void set_scene(scene::Scene* scene) { scene_ = scene; }

    // Play Mode 下只读展示，禁止修改字段与资源拖放
    void set_read_only(bool read_only) { read_only_ = read_only; }

    // 资源拖放回调：从 Project 面板拖文件到 Inspector 时触发（参数为 res:/ 路径）
    void set_drop_handler(std::function<void(scene::Entity*, const std::string&)> handler) {
        drop_handler_ = std::move(handler);
    }

    // 绑定 Undo/Redo 命令栈
    void set_undo_stack(CommandStack* stack) { undo_stack_ = stack; }

protected:
    void on_imgui() override;

private:
    // 由保存的 UUID 在当前场景中解析实体；若实体已被删除则返回 nullptr。
    scene::Entity* target_entity() const;

    void draw_component(scene::Entity* entity, components::Component* component);
    void draw_field(scene::Entity* entity, components::Component* component, const reflection::FieldInfo& field);
    void draw_material_section(scene::Entity* entity, components::Component* component);
    void draw_animation_section(scene::Entity* entity, components::Component* component);
    void draw_terrain_section(scene::Entity* entity, components::Component* component);
    void draw_particle_section(scene::Entity* entity, components::Component* component);
    void draw_physical_material_section(scene::Entity* entity, components::Component* component);

    std::string field_key(scene::Entity* entity, components::Component* component, const reflection::FieldInfo& field) const;
    void push_field_command(scene::Entity* entity, components::Component* component,
                            const reflection::FieldInfo& field,
                            const FieldValue& old_value,
                            const FieldValue& new_value);
    void track_field_change(scene::Entity* entity, components::Component* component,
                            const reflection::FieldInfo& field,
                            const FieldValue& current_value);

    scene::UUID selected_uuid_;
    scene::Scene* scene_ = nullptr;
    std::function<void(scene::Entity*, const std::string&)> drop_handler_;
    bool read_only_ = false;
    CommandStack* undo_stack_ = nullptr;

    // 正在编辑的字段旧值（entity_uuid:component_type:field_name -> FieldValue）
    std::unordered_map<std::string, FieldValue> active_fields_;

    // 材质编辑器窗口（Inspector 中点击 Edit Material 打开）
    MaterialEditorWindow material_editor_;
    // 动画编辑器窗口（Inspector 中 SkinnedMeshRenderer 打开）
    AnimationEditorWindow animation_editor_;
    // 地形编辑器窗口（Inspector 中 Terrain 打开）
    TerrainEditorWindow terrain_editor_;
    // 粒子编辑器窗口（Inspector 中 ParticleEmitter2D 打开）
    ParticleEditorWindow particle_editor_;
};

} // namespace gryce_engine::editor
