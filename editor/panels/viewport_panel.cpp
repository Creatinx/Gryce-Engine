#include "viewport_panel.h"

#include <ImGuizmo.h>

#include "components/transform.h"
#include "math/camera.h"
#include "render/imgui_backend.h"
#include "render/render_pipeline.h"
#include "scene/entity.h"
#include "../localization/localization.h"

namespace gryce_engine::editor {

namespace {

// 矩阵分解 TRS：平移取第 4 列，缩放取基向量长度，旋转归一化后转四元数。
// 负缩放（镜像）不处理，分解结果等价正缩放（编辑器 gizmo 不产生负缩放）。
void decompose_trs(const math::Matrix4f& m,
                   math::Vector3f& out_t, math::Quaternionf& out_r, math::Vector3f& out_s) {
    out_t = math::Vector3f(m(0, 3), m(1, 3), m(2, 3));

    math::Vector3f cx(m(0, 0), m(1, 0), m(2, 0));
    math::Vector3f cy(m(0, 1), m(1, 1), m(2, 1));
    math::Vector3f cz(m(0, 2), m(1, 2), m(2, 2));
    out_s = math::Vector3f(cx.length(), cy.length(), cz.length());

    if (out_s.x > 1e-8f) cx = cx / out_s.x;
    if (out_s.y > 1e-8f) cy = cy / out_s.y;
    if (out_s.z > 1e-8f) cz = cz / out_s.z;

    math::Matrix4f rot = math::Matrix4f::identity();
    rot(0, 0) = cx.x; rot(1, 0) = cx.y; rot(2, 0) = cx.z;
    rot(0, 1) = cy.x; rot(1, 1) = cy.y; rot(2, 1) = cy.z;
    rot(0, 2) = cz.x; rot(1, 2) = cz.y; rot(2, 2) = cz.z;
    out_r = math::Quaternionf::from_rotation_matrix(rot);
}

} // namespace

ViewportPanel::ViewportPanel() : EditorPanel("Viewport", "panel.viewport"), gizmo_operation_(ImGuizmo::TRANSLATE) {}

bool ViewportPanel::take_pick_click(ImVec2& out_uv) {
    if (!pick_click_pending_) return false;
    pick_click_pending_ = false;
    out_uv = pick_click_uv_;
    return true;
}

void ViewportPanel::on_imgui() {
    hovered_ = ImGui::IsWindowHovered();
    size_ = ImGui::GetContentRegionAvail();
    image_drawn_ = false;

    uint64_t tex_id = 0;
    if (pipeline_ && backend_) {
        tex_id = backend_->imgui_texture_id(pipeline_->viewport_color_texture());
    }

    if (tex_id != 0 && size_.x >= 1.0f && size_.y >= 1.0f) {
        image_min_ = ImGui::GetCursorScreenPos();
        // OpenGL FBO 颜色附件原点在左下，ImGui 图像原点在左上，翻转 V 轴
        ImGui::Image(ImTextureRef(static_cast<ImTextureID>(tex_id)), size_,
                     ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
        image_drawn_ = true;

        // 左击：记录拾取 UV（gizmo 交互中不上报，避免误拾取）
        if (editing_enabled_ && !gizmo_active_ &&
            ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const ImVec2 mouse = ImGui::GetMousePos();
            pick_click_uv_ = ImVec2((mouse.x - image_min_.x) / size_.x,
                                    (mouse.y - image_min_.y) / size_.y);
            pick_click_pending_ = true;
        }

        // 资源拖放目标：Project 面板拖拽文件到视口
        if (drop_handler_ && ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload =
                    ImGui::AcceptDragDropPayload("GRYCE_PROJECT_FILE", ImGuiDragDropFlags_AcceptBeforeDelivery)) {
                if (payload->IsDelivery() && payload->DataSize > 0) {
                    drop_handler_(std::string(static_cast<const char*>(payload->Data)));
                }
            }
            ImGui::EndDragDropTarget();
        }
    } else {
        ImGui::TextDisabled("%s", tr("viewport.texture_unavailable"));
        ImGui::TextDisabled("%s", tr("viewport.vulkan_not_implemented"));
        ImGui::TextDisabled("%s", tr("viewport.opengl_check_hdr"));
    }

    if (editing_enabled_ && image_drawn_) {
        handle_gizmo_shortcuts();
        draw_gizmo();
    } else {
        gizmo_active_ = false;
    }
}

void ViewportPanel::handle_gizmo_shortcuts() {
    // W/E/R 切换 gizmo 模式；与相机飞行键冲突，右键按住（飞行中）时不切换
    if (!hovered_ || ImGui::IsMouseDown(ImGuiMouseButton_Right)) return;
    if (ImGui::IsKeyPressed(ImGuiKey_W, false)) gizmo_operation_ = ImGuizmo::TRANSLATE;
    if (ImGui::IsKeyPressed(ImGuiKey_E, false)) gizmo_operation_ = ImGuizmo::ROTATE;
    if (ImGui::IsKeyPressed(ImGuiKey_R, false)) gizmo_operation_ = ImGuizmo::SCALE;
}

void ViewportPanel::draw_gizmo() {
    scene::Entity* entity = selection_provider_ ? selection_provider_() : nullptr;
    if (!entity || !camera_) {
        gizmo_active_ = false;
        return;
    }

    ImGuizmo::SetDrawlist();
    ImGuizmo::SetRect(image_min_.x, image_min_.y, size_.x, size_.y);

    math::Matrix4f view = camera_->get_view_matrix();
    math::Matrix4f proj = camera_->get_projection_matrix();
    math::Matrix4f world = entity->world_transform();

    ImGuizmo::Manipulate(view.m, proj.m,
                         static_cast<ImGuizmo::OPERATION>(gizmo_operation_),
                         ImGuizmo::WORLD, world.m);

    gizmo_active_ = ImGuizmo::IsUsing() || ImGuizmo::IsOver();

    if (ImGuizmo::IsUsing()) {
        // 写回 local transform：local = parent_world^-1 * new_world
        math::Matrix4f parent_world = math::Matrix4f::identity();
        if (scene::Entity* parent = entity->parent()) {
            parent_world = parent->world_transform();
        }
        const math::Matrix4f local = parent_world.inverse() * world;

        math::Vector3f t;
        math::Quaternionf r;
        math::Vector3f s;
        decompose_trs(local, t, r, s);

        components::Transform* tr = entity->transform();
        tr->position = t;
        tr->rotation = r;
        tr->scale = s;
    }
}

} // namespace gryce_engine::editor
