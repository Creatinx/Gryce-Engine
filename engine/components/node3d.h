#pragma once

#include "components/component.h"

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// Node3D — 标记实体为 3D 节点（Godot 风格）。
// 3D 节点的变换、层级由 Entity + Transform 提供；本组件补充 3D 可见性等属性。
// ---------------------------------------------------------------------------
class Node3D : public Component {
public:
    bool visible = true;

    Node3D() = default;

    const char* type() const override { return "Node3D"; }

    void serialize(nlohmann::json& out) const override {
        out["visible"] = visible;
    }

    void deserialize(const nlohmann::json& in) override {
        visible = in.value("visible", true);
    }
};

} // namespace gryce_engine::components
