#pragma once

#include "components/component.h"

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// StaticBody2D — 2D 静态刚体标记（不参与物理模拟运动）。
// ---------------------------------------------------------------------------
class StaticBody2D : public Component {
public:
    StaticBody2D() = default;

    const char* type() const override { return "StaticBody2D"; }

    void serialize(nlohmann::json& out) const override {
        (void)out;
    }

    void deserialize(const nlohmann::json& in) override {
        (void)in;
    }
};

} // namespace gryce_engine::components
