#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <string>

#include "components/component.h"
#include "math/math.h"
#include "render/render2d.h"

namespace gryce_engine::components::d2 {

// ---------------------------------------------------------------------------
// Transform2D — 2D 仿射变换（XY 平移 + Z 旋转 + XY 缩放）
// ---------------------------------------------------------------------------
struct Transform2D {
    math::Vector2f position = math::Vector2f::zero();
    float rotation = 0.0f;
    math::Vector2f scale = math::Vector2f::one();
};

// 计算实体的 2D 世界变换（Godot 风格父链组合）。
// 组合规则：
// - 从自身沿父链向上，逐层组合每个祖先 Transform 的 XY 平移 / Z 旋转 / XY 缩放
//   （无论祖先是否挂 Node2D；场景合成根节点的 Transform 为恒等，天然不影响结果）；
// - 若实体自身挂有 Node2D 且 top_level == true，则忽略整条父链（Godot top_level
//   语义：top_level 节点不受父级 2D 变换影响，世界变换 == 本地变换）；
// - 组合公式：world.pos = parent.pos + parent.rot * (parent.scale * local.pos)，
//   world.rot = parent.rot + local.rot，world.scale = parent.scale * local.scale。
Transform2D world_transform_2d(const scene::Entity* entity);


// ---------------------------------------------------------------------------
// 2D 渲染哈希辅助
// 用于 Dirty-Frame 优化：画面未变化时跳过本帧渲染。
// ---------------------------------------------------------------------------
inline uint64_t hash_float(float v) {
    uint32_t bits;
    static_assert(sizeof(bits) == sizeof(v), "float size mismatch");
    std::memcpy(&bits, &v, sizeof(v));
    return static_cast<uint64_t>(bits);
}

inline void hash_combine(uint64_t& seed, uint64_t value) {
    seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
}

inline uint64_t hash_string(const std::string& s) {
    return std::hash<std::string>{}(s);
}

// ---------------------------------------------------------------------------
// Component2D — 2D 组件基类
// 继承 Component，可挂载到 Entity 上。
// 位置/旋转/缩放来自 owner Entity 的 Transform 组件。
// ---------------------------------------------------------------------------
class Component2D : public Component {
public:
    // 2D 绘制顺序，数值越大越靠上（后绘制）。
    // Label 等 UI 元素应设在 ColorRect/Shape 之上。
    int render_order = 0;

    ~Component2D() override = default;

    virtual void draw(render::IRenderer2D* renderer) = 0;

    // 渲染状态哈希，用于 Dirty-Frame 优化。
    // 当组件所有影响画面的字段都没变化时，返回的哈希值应保持不变。
    virtual uint64_t render_hash() const;

    // 快捷访问 owner 的 2D 世界变换（沿父链组合，top_level 截止；见 world_transform_2d）
    math::Vector2f position() const;  // 世界 XY
    float rotation() const;           // 世界 Z 旋转
    math::Vector2f scale() const;     // 世界 XY 缩放

protected:
    Component2D() = default;

    // 序列化/反序列化 2D 组件公共字段（render_order、enabled）
    void serialize_base(nlohmann::json& out) const {
        out["enabled"] = enabled;
        out["render_order"] = render_order;
    }

    void deserialize_base(const nlohmann::json& in) {
        enabled = in.value("enabled", true);
        render_order = in.value("render_order", 0);
    }

    // 将本地点变换到世界（仅 position + scale，旋转后续扩展）
    [[nodiscard]] math::Vector2f transform_point(const math::Vector2f& local) const;
};

} // namespace gryce_engine::components::d2
