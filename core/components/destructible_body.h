#pragma once

#include "components/component.h"
#include "math/math.h"

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// DestructibleBody — 可破坏/可碎裂刚体组件。
// 当 RigidBody 记录的上一帧碰撞冲量超过阈值时，本体会被替换成若干碎片。
// ---------------------------------------------------------------------------
class DestructibleBody : public Component {
public:
    // 触发碎裂的最小碰撞冲量
    float fracture_threshold = 5.0f;

    // 碎裂时碎片沿爆炸方向获得的额外冲量
    float explosive_impulse = 2.0f;

    // 三个轴上的分段数（例如 {2,2,2} 产生 8 块）
    math::Vector3i segments = math::Vector3i(2, 2, 2);

    // 碎片数量上限，防止爆炸式增长
    int max_fragments = 64;

    // 碎片存活时间（秒），<=0 表示永久存活
    float fragment_lifetime = 5.0f;

    // 是否已碎裂（运行时状态，不序列化）
    bool fractured = false;

    DestructibleBody() = default;

    const char* type() const override { return "DestructibleBody"; }

    void serialize(nlohmann::json& out) const override {
        out["fracture_threshold"] = fracture_threshold;
        out["explosive_impulse"] = explosive_impulse;
        out["segments"] = { segments.x, segments.y, segments.z };
        out["max_fragments"] = max_fragments;
        out["fragment_lifetime"] = fragment_lifetime;
    }

    void deserialize(const nlohmann::json& in) override {
        fracture_threshold = in.value("fracture_threshold", 5.0f);
        explosive_impulse = in.value("explosive_impulse", 2.0f);
        auto s = in.value("segments", std::vector<int>{2, 2, 2});
        if (s.size() >= 3) segments = math::Vector3i(s[0], s[1], s[2]);
        max_fragments = in.value("max_fragments", 64);
        fragment_lifetime = in.value("fragment_lifetime", 5.0f);
    }
};

} // namespace gryce_engine::components
