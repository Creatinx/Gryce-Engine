#pragma once

#include "components/component.h"

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// FragmentBody — 碎片标记组件。
// 由 FractureSystem 生成，用于生命周期管理和后续清理。
// ---------------------------------------------------------------------------
class FragmentBody : public Component {
public:
    float lifetime = 5.0f;     // 总存活时间（秒），<=0 表示永久
    float time_alive = 0.0f;   // 已存活时间（运行时，不序列化）

    FragmentBody() = default;
    explicit FragmentBody(float life) : lifetime(life) {}

    const char* type() const override { return "FragmentBody"; }

    void serialize(nlohmann::json& out) const override {
        out["lifetime"] = lifetime;
    }

    void deserialize(const nlohmann::json& in) override {
        lifetime = in.value("lifetime", 5.0f);
    }
};

} // namespace gryce_engine::components
