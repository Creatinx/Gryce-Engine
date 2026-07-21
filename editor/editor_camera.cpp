#include "editor_camera.h"

#include <algorithm>

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
    camera_.set_position(math::Vector3f(0.0f, 0.0f, 5.0f));
}

void EditorCamera::set_move_speed(float speed) {
    move_speed_ = std::clamp(speed, k_min_speed, k_max_speed);
}

void EditorCamera::focus_on(const math::Vector3f& target, float distance) {
    camera_.set_position(target - camera_.forward() * distance);
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
