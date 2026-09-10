#pragma once

#include <functional>
#include <vector>

#include "scene/scene.h"
#include "scene/uuid.h"

namespace gryce_engine::editor {

class CommandStack;

// ---------------------------------------------------------------------------
// HierarchyPanel — 场景实体树
// ---------------------------------------------------------------------------
class HierarchyPanel {
public:
    using SelectionChangedCallback = std::function<void(scene::Entity*)>;

    void set_scene(scene::Scene* scene) { scene_ = scene; }
    void set_command_stack(CommandStack* stack) { command_stack_ = stack; }
    void set_on_selection_changed(SelectionChangedCallback cb) {
        on_selection_changed_ = std::move(cb);
    }

    scene::UUID selected_uuid() const { return selected_uuid_; }
    void set_selected_uuid(const scene::UUID& id) { selected_uuid_ = id; }
    scene::Entity* selected_entity() const;

    const std::vector<scene::Entity*>& queued_deletes() const { return queued_deletes_; }
    void clear_queued_deletes() { queued_deletes_.clear(); }

    void render();

private:
    void render_entity(scene::Entity* entity, int depth);
    void render_entity_context_menu(scene::Entity* entity);
    void create_entity(scene::Entity* parent);
    void queue_delete(scene::Entity* entity);
    void reparent_entity(scene::Entity* entity, scene::Entity* new_parent);
    bool is_descendant_of(scene::Entity* entity, scene::Entity* ancestor) const;

    scene::Scene* scene_ = nullptr;
    CommandStack* command_stack_ = nullptr;
    scene::UUID selected_uuid_ = scene::UUID::nil();
    SelectionChangedCallback on_selection_changed_;
    std::vector<scene::Entity*> queued_deletes_;
};

} // namespace gryce_engine::editor
