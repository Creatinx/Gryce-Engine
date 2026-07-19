#include "editor_camera.h"

#include <algorithm>

#include <imgui.h>

#include "utils/glog/glog_lib.h"

namespace gryce_engine::editor {

namespace {
constexpr float k_min_speed = 0.1f;
constexpr float k_max_speed = 100.0f;
constexpr float k_wheel_step = 1.15f; // 滚轮每格速度倍率
} // namespace

EditorCamera::EditorCamera() {
    camera_.set_position(math::Vector3f(0.0f, 0.0f, 5.0f));
}

void EditorCamera::set_move_speed(float speed) {
    move_speed_ = std::clamp(speed, k_min_speed, k_max_speed);
}

void EditorCamera::update(float dt, bool viewport_hovered) {
    if (!viewport_hovered) return;

    ImGuiIO& io = ImGui::GetIO();

    // 滚轮调速（不要求右键按住）
    if (io.MouseWheel != 0.0f) {
        const float factor = io.MouseWheel > 0.0f ? k_wheel_step : 1.0f / k_wheel_step;
        set_move_speed(move_speed_ * factor);
    }

    // F 聚焦（占位）：后续接入选中实体的包围盒，把相机移到合适距离
    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
        GLOG_DEBUG("EditorCamera: focus (F) pressed - placeholder, not implemented yet");
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
