#pragma once

#include "export.h"
#include "ecs/system.h"

namespace gryce_engine::ecs {

// ---------------------------------------------------------------------------
// HierarchySystem — ECS 层级维护系统
//
// 职责：
//   - 每帧在 PostUpdate 阶段同步 ParentComponent / ChildrenComponent 数据
//   - 维护全局变换缓存（world_transform 的 ECS 版本）
//   - 处理层级变更后的脏标记传播
//
// 此系统使层级数据对 ECS 组件池查询可见，未来可在此做：
//   - 变换脏标记的批量传播
//   - 全局变换矩阵的惰性计算
//   - 层级变更事件的统一派发
// ---------------------------------------------------------------------------
class GRYCE_API HierarchySystem : public ISystem {
public:
    const char* name() const override { return "HierarchySystem"; }
    Phase phase() const override { return Phase::PostUpdate; }

    void on_update(scene::Scene& scene, float dt) override;
};

} // namespace gryce_engine::ecs