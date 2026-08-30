#pragma once

#include <string>
#include <vector>

#include "components/component.h"
#include "scene/scene.h"

namespace gryce_engine::reflection {
struct FieldInfo;
}

namespace gryce_engine::editor {

class HierarchyPanel;
class CommandStack;

// ---------------------------------------------------------------------------
// InspectorPanel — 通过 reflection 动态编辑选中实体组件
// ---------------------------------------------------------------------------
class InspectorPanel {
public:
    void set_scene(scene::Scene* scene) { scene_ = scene; }
    void set_hierarchy(HierarchyPanel* hierarchy) { hierarchy_ = hierarchy; }
    void set_command_stack(CommandStack* stack) { command_stack_ = stack; }
    void set_play_mode(bool playing) { play_mode_ = playing; }

    void render();

private:
    scene::Entity* selected_entity() const;
    void render_entity_header(scene::Entity* entity);
    void render_component(scene::Entity* entity, components::Component* component);
    void render_field(components::Component* component,
                      const reflection::FieldInfo& field);
    void draw_add_component_popup(scene::Entity* entity);
    void queue_remove_component(components::Component* component);
    bool is_internal_component_type(const std::string& type) const;
    bool is_field_read_only(const components::Component* component,
                            const reflection::FieldInfo& field) const;

    scene::Scene* scene_ = nullptr;
    HierarchyPanel* hierarchy_ = nullptr;
    CommandStack* command_stack_ = nullptr;
    bool play_mode_ = false;
    std::vector<components::Component*> queued_component_removals_;
};

} // namespace gryce_engine::editor
