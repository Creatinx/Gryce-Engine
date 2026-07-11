#pragma once

#include "components/component.h"

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// StaticBody — 静态刚体占位组件（不参与物理模拟运动）。
// ---------------------------------------------------------------------------
class StaticBody : public Component {
public:
    bool kinematic = false;

    StaticBody() = default;

    const char* type() const override { return "StaticBody"; }

    void serialize(nlohmann::json& out) const override {
        out["kinematic"] = kinematic;
    }

    void deserialize(const nlohmann::json& in) override {
        kinematic = in.value("kinematic", false);
    }
};

} // namespace gryce_engine::components
