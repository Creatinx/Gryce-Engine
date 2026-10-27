#pragma once

#include "export.h"
#include "math/math.h"

namespace gryce_engine::math {

// ---------------------------------------------------------------------------
// Camera — FPS 自由摄像机
// 主线程更新位置/旋转，渲染线程使用 view/projection 矩阵
// ---------------------------------------------------------------------------
class GRYCE_API Camera {
public:
    Camera();

    // 更新位置（基于输入和 delta_time）
    void update(float delta_time,
                bool move_forward, bool move_backward,
                bool move_left, bool move_right,
                bool move_up, bool move_down,
                bool sprint,
                float mouse_delta_x, float mouse_delta_y);

    Matrix4f get_view_matrix() const;
    Matrix4f get_projection_matrix(float aspect) const;
    Matrix4f get_projection_matrix() const; // 使用当前存储的 aspect

    Vector3f position() const { return position_; }
    Vector3f forward() const { return forward_; }
    Vector3f right() const { return right_; }
    Vector3f up() const { return up_; }
    float yaw() const { return yaw_; }
    float pitch() const { return pitch_; }

    void set_position(const Vector3f& pos) { position_ = pos; }
    void set_yaw(float yaw) { yaw_ = yaw; update_vectors(); }
    void set_pitch(float pitch) { pitch_ = math::clamp(pitch, -89.0f, 89.0f); update_vectors(); }

    void set_aspect(float aspect) { aspect_ = aspect; }
    float aspect() const { return aspect_; }

    void set_move_speed(float speed) { move_speed_ = speed; }
    void set_sprint_multiplier(float mult) { sprint_multiplier_ = mult; }
    void set_mouse_sensitivity(float sens) { mouse_sensitivity_ = sens; }
    void set_fov(float fov) { fov_ = fov; }
    float fov() const { return fov_; }

    // 让相机看向目标点，并重新计算 yaw/pitch/forward/right/up。
    void look_at(const Vector3f& target);
    void set_near_far(float near_plane, float far_plane) { near_ = near_plane; far_ = far_plane; }
    float near_plane() const { return near_; }
    float far_plane() const { return far_; }

    // 位置/朝向不连续跳变标记（场景切换、传送、镜头剪切）。供运动矢量的
    // 消费方判断"上一帧矩阵与当前帧不可比"：置位后当帧应把运动矢量视为零，
    // 或向 DLSS 类时域重建传递 reset 标记。
    // 注意：不在 set_position/set_yaw/set_pitch/look_at 里自动置位——场景相机
    // 每帧都从 Transform 重建并走这些 setter（GEntity_GetCamera 的同步路径），
    // 自动置位会让运动矢量永远失效。只有显式 mark_discontinuity 才算跳变。
    void mark_discontinuity() { discontinuity_ = true; }
    bool has_discontinuity() const { return discontinuity_; }
    void clear_discontinuity() { discontinuity_ = false; }

private:
    void update_vectors();

    Vector3f position_;
    Vector3f forward_;
    Vector3f right_;
    Vector3f up_;

    float yaw_   = -90.0f;   // 偏航角（度），-90 看向 -Z
    float pitch_ = 0.0f;     // 俯仰角（度）
    float fov_   = 60.0f;    // 垂直 FOV（度）
    float near_  = 0.1f;
    float far_   = 100.0f;
    float aspect_ = 16.0f / 9.0f;

    float move_speed_         = 5.0f;
    float sprint_multiplier_  = 2.5f;
    float mouse_sensitivity_  = 0.1f;

    bool discontinuity_ = false;
};

} // namespace gryce_engine::math
