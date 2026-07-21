#pragma once

#include "math/camera.h"

namespace gryce_engine::editor {

// ---------------------------------------------------------------------------
// EditorCamera — 编辑器自由飞行相机（M1-E1）
// 不依赖场景相机实体，内部复用 math::Camera 的矩阵/向量计算。
// 输入走 ImGui IO，只在 Viewport 面板悬停时响应：
//   右键按住拖动   —— 旋转视角（yaw/pitch）
//   W/A/S/D + Q/E  —— 平移（需右键按住，Q 降 E 升），Shift 加速
//   滚轮           —— 沿视线方向缩放（推进/拉远）
//   F              —— 聚焦选中实体（占位，待接入场景边界盒）
// ---------------------------------------------------------------------------
class EditorCamera {
public:
    EditorCamera();

    // 每帧更新；viewport_hovered 为 Viewport 面板悬停状态
    void update(float dt, bool viewport_hovered);

    // 聚焦目标：把相机移到目标前方固定距离，保持当前朝向
    void focus_on(const math::Vector3f& target, float distance = 10.0f);

    math::Camera& camera() { return camera_; }
    const math::Camera& camera() const { return camera_; }

    float move_speed() const { return move_speed_; }
    void set_move_speed(float speed);

private:
    math::Camera camera_;
    float move_speed_ = 5.0f;
    float look_sensitivity_ = 0.15f;  // 度/像素
    float sprint_multiplier_ = 3.0f;
};

} // namespace gryce_engine::editor
