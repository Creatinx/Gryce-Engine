#pragma once

#include "camera_preset.h"
#include "math/camera.h"

namespace gryce_engine::editor {

// ---------------------------------------------------------------------------
// EditorCamera — 编辑器视口相机（M1-E1）
// 不依赖场景相机实体，内部复用 math::Camera 的矩阵/向量计算。
// 输入走 ImGui IO，只在 Viewport 面板悬停时响应：
//   右键按住拖动   —— 旋转视角（yaw/pitch）
//   滚轮           —— 沿视线方向推进/拉远（移动视角）
//   F              —— 聚焦选中实体（按 MeshRenderer/SkinnedMeshRenderer 的世界 AABB）
//   （不使用 WASD 平移移动视角；移动仅通过滚轮沿视线方向）
//
// 相机预设：
//   Static    — 固定视角，仅响应手动输入
//   Orbit     — 自动绕场景中心旋转展示
//   Flythrough — 自由飞行（手动输入）
//   Demo      — 播放场景内置相机动画（当前回退为静态）
// ---------------------------------------------------------------------------

class EditorCamera {
public:
    EditorCamera();

    // 每帧更新；viewport_hovered 为 Viewport 面板悬停状态
    void update(float dt, bool viewport_hovered);

    // 聚焦目标：把相机移到目标前方固定距离，保持当前朝向
    void focus_on(const math::Vector3f& target, float distance = 10.0f);

    // 按包围球聚焦：根据相机 FOV 自动计算合适距离，使目标完整出现在视野内。
    void focus_on_bounds(const math::Vector3f& center, float radius);

    math::Camera& camera() { return camera_; }
    const math::Camera& camera() const { return camera_; }

    float move_speed() const { return move_speed_; }
    void set_move_speed(float speed);

    void set_preset(CameraPreset preset) { preset_ = preset; }
    CameraPreset preset() const { return preset_; }

    // 设置轨道旋转中心（Orbit 模式使用）
    void set_orbit_target(const math::Vector3f& target) { orbit_target_ = target; }
    const math::Vector3f& orbit_target() const { return orbit_target_; }

    // 设置轨道半径与速度
    void set_orbit_radius(float radius);
    void set_orbit_speed(float degrees_per_second) { orbit_speed_ = degrees_per_second; }

private:
    void update_orbit(float dt);

    math::Camera camera_;
    float move_speed_ = 5.0f;
    float look_sensitivity_ = 0.15f;  // 度/像素

    CameraPreset preset_ = CameraPreset::Static;

    // Orbit 状态
    math::Vector3f orbit_target_ = math::Vector3f::zero();
    float orbit_radius_ = 10.0f;
    float orbit_speed_ = 15.0f; // 度/秒
    float orbit_angle_ = 0.0f;
};

} // namespace gryce_engine::editor
