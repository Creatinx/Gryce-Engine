#include "editor_camera.h"

#include <algorithm>
#include <cmath>

#include <imgui.h>

#include "utils/glog/glog_lib.h"

namespace gryce_engine::editor {

namespace {
constexpr float k_min_speed = 0.1f;
constexpr float k_max_speed = 100.0f;
constexpr float k_zoom_step = 2.0f;   // 滚轮每格缩放移动距离
constexpr float k_min_distance = 0.1f; // 距离原点最近限制，防止穿到另一侧
constexpr float k_max_distance = 500.0f;
} // namespace

EditorCamera::EditorCamera() {
    // 默认从斜上方看向原点，避免一打开场景就处在地面/物体下方导致“画面反了”的错觉。
    camera_.set_position(math::Vector3f(0.0f, 10.0f, 10.0f));
    camera_.set_pitch(-45.0f);
}

void EditorCamera::set_move_speed(float speed) {
    move_speed_ = std::clamp(speed, k_min_speed, k_max_speed);
}

void EditorCamera::focus_on(const math::Vector3f& target, float distance) {
    camera_.set_position(target - camera_.forward() * distance);
}

void EditorCamera::focus_on_bounds(const math::Vector3f& center, float radius) {
    if (radius <= 0.0f) {
        focus_on(center);
        return;
    }
    // 让包围球在垂直方向占满视锥的 70%，留点边缘余量。
    const float fov_rad = math::to_radians(camera_.fov());
    const float distance = std::max(radius / std::tan(fov_rad * 0.35f), k_min_distance);

    // 从斜上方 45° 看向目标中心，避免相机被放到目标下方还朝上看的尴尬情况。
    // 同时保留当前水平朝向（yaw），只调整 pitch，让过渡更自然。
    const math::Vector3f flat_forward = math::Vector3f(
        std::cos(math::to_radians(camera_.yaw())),
        0.0f,
        std::sin(math::to_radians(camera_.yaw()))).normalized();
    // 若水平前向量为零（理论上不会发生），退化为 +Z 方向。
    const math::Vector3f safe_forward = flat_forward.length_sq() > 1e-6f ? flat_forward : math::Vector3f(0.0f, 0.0f, 1.0f);

    // 水平距离 = distance * cos(45°)，垂直距离 = distance * sin(45°)。
    const float horizontal_dist = distance * 0.70710678f;
    const float vertical_dist = distance * 0.70710678f;
    camera_.set_position(center - safe_forward * horizontal_dist + math::Vector3f(0.0f, vertical_dist, 0.0f));

    // 让相机看向目标中心。
    const math::Vector3f look_dir = (center - camera_.position()).normalized();
    const float pitch = math::to_degrees(std::asin(math::clamp(look_dir.y, -1.0f, 1.0f)));
    const float yaw = math::to_degrees(std::atan2(look_dir.z, look_dir.x));
    camera_.set_pitch(pitch);
    camera_.set_yaw(yaw);
}

void EditorCamera::update(float dt, bool viewport_hovered) {
    if (!viewport_hovered) return;

    ImGuiIO& io = ImGui::GetIO();

    // 滚轮缩放（不要求右键按住）：沿相机前方向推进/拉远
    if (io.MouseWheel != 0.0f) {
        const math::Vector3f new_pos = camera_.position() + camera_.forward() * io.MouseWheel * k_zoom_step;
        const float dist = new_pos.length();
        if (dist >= k_min_distance && dist <= k_max_distance) {
            camera_.set_position(new_pos);
        }
    }

    // 视角旋转 + 平移只在右键按住时生效（类 Unity 编辑器交互）
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) return;

    camera_.set_yaw(camera_.yaw() + io.MouseDelta.x * look_sensitivity_);
    camera_.set_pitch(camera_.pitch() - io.MouseDelta.y * look_sensitivity_);

    math::Vector3f move = math::Vector3f::zero();
    if (ImGui::IsKeyDown(ImGuiKey_W)) move += camera_.forward();
    if (ImGui::IsKeyDown(ImGuiKey_S)) move -= camera_.forward();
    if (ImGui::IsKeyDown(ImGuiKey_D)) move += camera_.right();
    if (ImGui::IsKeyDown(ImGuiKey_A)) move -= camera_.right();
    if (ImGui::IsKeyDown(ImGuiKey_E)) move += math::Vector3f(0.0f, 1.0f, 0.0f);
    if (ImGui::IsKeyDown(ImGuiKey_Q)) move -= math::Vector3f(0.0f, 1.0f, 0.0f);

    if (move.length_sq() > 0.0f) {
        const float speed = move_speed_ *
                            (ImGui::IsKeyDown(ImGuiKey_LeftShift) ? sprint_multiplier_ : 1.0f);
        camera_.set_position(camera_.position() + move.normalized() * speed * dt);
    }
}

} // namespace gryce_engine::editor
