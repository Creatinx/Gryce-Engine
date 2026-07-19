#pragma once

#include "ecs/system.h"

namespace gryce_engine::ecs {

// ---------------------------------------------------------------------------
// AnimatorSystem — 骨骼动画驱动系统
//
// Update 阶段对每个 SkinnedMeshRenderer：
//   1. 推进播放时间（speed 缩放；loop/clamp 由采样层处理）
//   2. evaluate_skin_palette 求当前帧 palette
//   3. 新分配的 shared_ptr<vector<Matrix4f>> 注入组件（渲染命令按值捕获，
//      主线程替换指针不影响已入队命令，无跨线程数据竞争）
//
// 多 clip 切换最小支持：修改组件 clip_name 即生效（resolve_clip 按名解析）。
// 状态机 / 混合（crossfade）不在本轮（M4）。
// ---------------------------------------------------------------------------
class AnimatorSystem : public ISystem {
public:
    const char* name() const override { return "AnimatorSystem"; }
    Phase phase() const override { return Phase::Update; }

    void on_update(scene::Scene& scene, float dt) override;
};

} // namespace gryce_engine::ecs
