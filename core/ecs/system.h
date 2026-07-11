#pragma once

#include <string>
#include <typeindex>
#include <vector>

#include "ecs/types.h"

namespace gryce_engine {
namespace scene { class Scene; }
namespace render { class RenderContext; }
} // namespace gryce_engine

namespace gryce_engine::ecs {

// ---------------------------------------------------------------------------
// ISystem — ECS System 基类
// System 不持有状态（或只持有全局状态），每帧处理满足组件要求的 Entity。
// ---------------------------------------------------------------------------
class ISystem {
public:
    virtual ~ISystem() = default;

    // System 名称，用于调试和查询
    virtual const char* name() const = 0;

    // System 执行阶段（按顺序驱动）
    enum class Phase {
        PreUpdate,  // 早期逻辑（输入、动画前期）
        Update,     // 主逻辑更新
        PostUpdate, // 后期逻辑（物理后处理、动画后期）
        PreRender,  // 渲染准备（剔除、排序）
        Render,     // 主渲染
        PostRender, // 渲染后处理（UI、调试绘制）
    };

    virtual Phase phase() const { return Phase::Update; }

    // 优先级：同 phase 下数值越大越先执行（默认 0）
    virtual int priority() const { return 0; }

    // 声明本 System 感兴趣的组件类型（空表示处理所有 Entity）
    virtual std::vector<ComponentTypeID> required_components() const { return {}; }

    // 生命周期
    virtual void on_init(scene::Scene& scene) { (void)scene; }
    virtual void on_shutdown(scene::Scene& scene) { (void)scene; }

    // 每帧调用
    virtual void on_update(scene::Scene& scene, float dt) { (void)scene; (void)dt; }
    virtual void on_render(scene::Scene& scene, render::RenderContext& ctx) { (void)scene; (void)ctx; }

    bool enabled = true;
};

} // namespace gryce_engine::ecs
