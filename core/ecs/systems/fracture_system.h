#pragma once

#include "ecs/system.h"

namespace gryce_engine::ecs {

// ---------------------------------------------------------------------------
// FractureSystem — 碎裂系统
// 在 PhysicsSystem 之后运行，处理带 DestructibleBody 的 Entity 的碎裂。
// ---------------------------------------------------------------------------
class FractureSystem : public ISystem {
public:
    const char* name() const override { return "FractureSystem"; }
    Phase phase() const override { return Phase::PostUpdate; }

    void on_update(scene::Scene& scene, float dt) override;
};

} // namespace gryce_engine::ecs
