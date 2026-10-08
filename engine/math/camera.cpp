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
    float cy = std::cos(yaw_rad), sy = std::sin(yaw_rad);
    float cp = std::cos(pitch_rad), sp = std::sin(pitch_rad);

    // 球面坐标构造的 forward 已为单位向量（cy²+sy²=1 ⇒ 模长为 1），无需 normalize。
    forward_ = Vector3f(cy * cp, sp, sy * cp);
    // right = forward × up，需一次 normalize（模长为 |cos pitch|，pitch∈(-90,90) 不退化）。
    right_   = forward_.cross(Vector3f::up()).normalized();
    // up = right × forward，right、forward 同为单位且正交，结果已为单位向量，无需 normalize。
    up_      = right_.cross(forward_);
}

void Camera::look_at(const Vector3f& target) {
    Vector3f dir = (target - position_).normalized();
    pitch_ = math::to_degrees(std::asin(math::clamp(dir.y, -1.0f, 1.0f)));
    yaw_ = math::to_degrees(std::atan2(dir.z, dir.x));
    update_vectors();
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
