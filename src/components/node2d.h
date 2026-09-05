#pragma once

#include "components/component.h"

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// Node2D — 标记实体为 2D 节点（Godot 风格）。
// 2D 节点的变换、层级仍由 Entity + Transform 提供；本组件仅补充 2D 专属属性。
// ---------------------------------------------------------------------------
class Node2D : public Component {
public:
    // 绘制/逻辑层级，数值越大越靠上（后处理）。
    int z_index = 0;

    // 若为 true，则忽略父级 2D 变换影响（Godot 的 top_level）。
    bool top_level = false;

    Node2D() = default;

    const char* type() const override { return "Node2D"; }

    void serialize(nlohmann::json& out) const override {
        out["z_index"] = z_index;
        out["top_level"] = top_level;
    }

    void deserialize(const nlohmann::json& in) override {
        z_index = in.value("z_index", 0);
        top_level = in.value("top_level", false);
    }
};

} // namespace gryce_engine::components
