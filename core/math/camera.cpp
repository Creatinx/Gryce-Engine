#include "camera.h"

#include <cmath>

namespace gryce_engine::math {

Camera::Camera()
    : position_(0.0f, 0.0f, 5.0f)
    , forward_(0.0f, 0.0f, -1.0f)
    , right_(1.0f, 0.0f, 0.0f)
    , up_(0.0f, 1.0f, 0.0f) {}

void Camera::update(float delta_time,
                    bool move_forward, bool move_backward,
                    bool move_left, bool move_right,
                    bool move_up, bool move_down,
                    bool sprint,
                    float mouse_delta_x, float mouse_delta_y) {
    float speed = move_speed_ * (sprint ? sprint_multiplier_ : 1.0f);

    Vector3f move(0.0f, 0.0f, 0.0f);
    if (move_forward)  move += forward_;
    if (move_backward) move -= forward_;
    if (move_left)     move -= right_;
    if (move_right)    move += right_;
    if (move_up)       move += Vector3f::up();
    if (move_down)     move -= Vector3f::up();

    if (move.length_sq() > 0.0f) {
        position_ += move.normalized() * speed * delta_time;
    }

    yaw_   += mouse_delta_x * mouse_sensitivity_;
    pitch_ += mouse_delta_y * mouse_sensitivity_;
    pitch_ = math::clamp(pitch_, -89.0f, 89.0f);

    update_vectors();
}

void Camera::update_vectors() {
    float yaw_rad   = math::to_radians(yaw_);
    float pitch_rad = math::to_radians(pitch_);

    forward_.x = std::cos(yaw_rad) * std::cos(pitch_rad);
    forward_.y = std::sin(pitch_rad);
    forward_.z = std::sin(yaw_rad) * std::cos(pitch_rad);
    forward_ = forward_.normalized();

    right_ = forward_.cross(Vector3f::up()).normalized();
    up_    = right_.cross(forward_).normalized();
}

Matrix4f Camera::get_view_matrix() const {
    return Matrix4f::look_at(position_, position_ + forward_, up_);
}

Matrix4f Camera::get_projection_matrix(float aspect) const {
    return Matrix4f::perspective(math::to_radians(fov_), aspect, near_, far_);
}

Matrix4f Camera::get_projection_matrix() const {
    return Matrix4f::perspective(math::to_radians(fov_), aspect_, near_, far_);
}

} // namespace gryce_engine::math
