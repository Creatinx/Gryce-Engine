#include "components/physical_material.h"

namespace gryce_engine::components {

void PhysicalMaterial::apply_preset(const std::string& name) {
    for (int i = 0; i < k_physical_material_preset_count; ++i) {
        if (name == k_physical_material_presets[i].name) {
            preset_name = name;
            softness = k_physical_material_presets[i].softness;
            drag_coefficient = k_physical_material_presets[i].drag_coefficient;
            density = k_physical_material_presets[i].density;
            friction = k_physical_material_presets[i].friction;
            return;
        }
    }
}

} // namespace gryce_engine::components
