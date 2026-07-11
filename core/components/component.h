#pragma once

#include <nlohmann/json.hpp>

namespace gryce_engine::scene {
    class Entity;
} // namespace gryce_engine::scene

namespace gryce_engine::render {
    class RenderContext;
} // namespace gryce_engine::render

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// Component — 所有组件的基类
// 采用 Unity 风格：组件挂载到 Entity 上，可序列化。
// ---------------------------------------------------------------------------
class Component {
public:
    virtual ~Component() = default;

    // 组件类型标识，用于反序列化工厂
    virtual const char* type() const = 0;

    // 序列化/反序列化
    virtual void serialize(nlohmann::json& out) const = 0;
    virtual void deserialize(const nlohmann::json& in) = 0;

    // 生命周期
    virtual void on_attach(scene::Entity* owner) { owner_ = owner; }
    virtual void on_detach() { owner_ = nullptr; }
    virtual void on_init() {}                       // 场景初始化完成后调用
    virtual void on_update(float dt) { (void)dt; }  // 每帧逻辑更新
    virtual void on_render(render::RenderContext& ctx) { (void)ctx; } // 每帧渲染（可选）
    virtual void on_destroy() {}                    // 组件销毁前

    scene::Entity* owner() const { return owner_; }

    bool enabled = true;

protected:
    Component() = default;

    scene::Entity* owner_ = nullptr;
};

} // namespace gryce_engine::components
