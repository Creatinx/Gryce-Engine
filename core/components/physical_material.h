#pragma once

#include <string>
#include <vector>

#include "components/component.h"

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// PhysicalMaterialPreset — 物理材质预设
// 用于 3D/2D 物体统一描述“软度、风阻、密度”等物理属性。
// ---------------------------------------------------------------------------
struct PhysicalMaterialPreset {
    const char* name;
    float softness;          // 0=完全硬/弹性，1=极软/吸能
    float drag_coefficient;  // 0=无风阻，1=极大风阻
    float density;           // 相对密度（水的密度为 1.0）
    float friction;          // 0=无摩擦，1=极大摩擦
};

constexpr PhysicalMaterialPreset k_physical_material_presets[] = {
    // name,       softness, drag,  density, friction
    {"Wood",     0.20f, 0.15f, 0.70f, 0.55f},
    {"Stone",    0.05f, 0.05f, 2.50f, 0.75f},
    {"Metal",    0.05f, 0.08f, 7.80f, 0.40f},
    {"Rubber",   0.15f, 0.25f, 1.10f, 0.85f},
    {"Ice",      0.20f, 0.02f, 0.92f, 0.05f},
    {"Concrete", 0.15f, 0.12f, 2.40f, 0.70f},
    {"Fabric",   0.90f, 0.55f, 0.15f, 0.90f},
    {"Glass",    0.05f, 0.03f, 2.50f, 0.10f},
};
constexpr int k_physical_material_preset_count =
    static_cast<int>(sizeof(k_physical_material_presets) / sizeof(k_physical_material_presets[0]));

// ---------------------------------------------------------------------------
// PhysicalMaterial — 物理材质组件
// 可挂载到任意 Entity，与渲染材质解耦，2D/3D 通用。
// ---------------------------------------------------------------------------
class PhysicalMaterial : public Component {
public:
    std::string preset_name;
    float softness = 0.0f;          // 0~1
    float drag_coefficient = 0.0f;  // 0~1
    float density = 1.0f;           // 相对密度
    float friction = 0.5f;          // 0~1

    PhysicalMaterial() = default;

    const char* type() const override { return "PhysicalMaterial"; }

    void serialize(nlohmann::json& out) const override {
        out["preset_name"] = preset_name;
        out["softness"] = softness;
        out["drag_coefficient"] = drag_coefficient;
        out["density"] = density;
        out["friction"] = friction;
    }

    void deserialize(const nlohmann::json& in) override {
        preset_name = in.value("preset_name", preset_name);
        softness = in.value("softness", softness);
        drag_coefficient = in.value("drag_coefficient", drag_coefficient);
        density = in.value("density", density);
        friction = in.value("friction", friction);
        // 如果保存时带有预设名，加载后重新同步预设参数，
        // 这样修改代码里的预设表会自动更新旧场景。
        if (!preset_name.empty()) {
            apply_preset(preset_name);
        }
    }

    // 应用预设；name 不存在时保持当前值
    void apply_preset(const std::string& name);

    // 由软度推导弹性（restitution）
    float restitution() const { return 1.0f - softness; }

    // 给定环境重力加速度，计算考虑风阻后的等效重力加速度
    float effective_gravity(float world_gravity) const {
        return world_gravity * (1.0f - drag_coefficient);
    }
};

} // namespace gryce_engine::components
