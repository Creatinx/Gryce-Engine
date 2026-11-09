#include "components/node.h"

#include "scene/entity.h"

namespace gryce_engine::components {

bool Node::is_visible_in_tree() const {
    if (!visible || !is_effectively_enabled()) return false;

    for (const Component* cur = component_parent(); cur; cur = cur->component_parent()) {
        if (const auto* node = dynamic_cast<const Node*>(cur)) {
            if (!node->visible) return false;
        }
    }

    for (const scene::Entity* entity = owner(); entity; entity = entity->parent()) {
        if (!entity->enabled) return false;
        const auto* node = entity->get_component<Node>();
        if (node && node != this && !node->visible) return false;
    }

    return true;
}

} // namespace gryce_engine::components
