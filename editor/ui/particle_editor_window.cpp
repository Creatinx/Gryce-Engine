#include "particle_editor_window.h"

#include <algorithm>
#include <cmath>

#include <imgui.h>

#include "scene/entity.h"
#include "utils/glog/glog_lib.h"
#include "../localization/localization.h"

namespace gryce_engine::editor {

namespace {

render::Color lerp_color(const render::Color& a, const render::Color& b, float t) {
    return render::Color(
        a.r + (b.r - a.r) * t,
        a.g + (b.g - a.g) * t,
        a.b + (b.b - a.b) * t,
        a.a + (b.a - a.a) * t
    );
}

} // namespace

void ParticleEditorWindow::open(scene::Entity* entity, components::d2::ParticleEmitter2D* emitter) {
    entity_ = entity;
    emitter_ = emitter;
    open_ = (entity != nullptr && emitter != nullptr);
}

void ParticleEditorWindow::draw() {
    if (!open_ || !emitter_ || !entity_) return;

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(460.0f, 720.0f), ImGuiCond_Appearing);

    bool still_open = true;
    if (ImGui::Begin(tr("particle_editor.title"), &still_open, ImGuiWindowFlags_NoDocking)) {
        ImGui::Text("%s: %s", tr("common.name"), entity_->name().c_str());
        ImGui::Separator();

        draw_emission();
        draw_lifetime();
        draw_velocity();
        draw_appearance();
        ImGui::Separator();
        draw_preview();
        ImGui::Separator();
        draw_actions();
    }
    ImGui::End();

    if (!still_open) {
        open_ = false;
        entity_ = nullptr;
        emitter_ = nullptr;
    }
}

void ParticleEditorWindow::draw_emission() {
    if (ImGui::CollapsingHeader(tr("particle_editor.emission"), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::DragFloat(tr("particle_editor.emission_rate"), &emitter_->emission_rate, 1.0f, 0.0f, 10000.0f);
        ImGui::DragInt(tr("particle_editor.max_particles"), &emitter_->max_particles, 1, 1, 100000);

        ImGui::Text("%s", tr("particle_editor.burst"));
        ImGui::DragInt(tr("particle_editor.burst_min"), &emitter_->burst_min, 1, 0, 10000);
        ImGui::DragInt(tr("particle_editor.burst_max"), &emitter_->burst_max, 1, 0, 10000);
    }
}

void ParticleEditorWindow::draw_lifetime() {
    if (ImGui::CollapsingHeader(tr("particle_editor.lifetime"), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::DragFloat(tr("particle_editor.lifetime_min"), &emitter_->lifetime_min, 0.05f, 0.01f, 60.0f);
        ImGui::DragFloat(tr("particle_editor.lifetime_max"), &emitter_->lifetime_max, 0.05f, 0.01f, 60.0f);
        if (emitter_->lifetime_min > emitter_->lifetime_max) {
            std::swap(emitter_->lifetime_min, emitter_->lifetime_max);
        }
    }
}

void ParticleEditorWindow::draw_velocity() {
    if (ImGui::CollapsingHeader(tr("particle_editor.velocity"), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::DragFloat(tr("particle_editor.velocity_min"), &emitter_->velocity_min, 1.0f, 0.0f, 10000.0f);
        ImGui::DragFloat(tr("particle_editor.velocity_max"), &emitter_->velocity_max, 1.0f, 0.0f, 10000.0f);
        if (emitter_->velocity_min > emitter_->velocity_max) {
            std::swap(emitter_->velocity_min, emitter_->velocity_max);
        }

        float dir_min_deg = emitter_->direction_min * 180.0f / 3.14159265f;
        float dir_max_deg = emitter_->direction_max * 180.0f / 3.14159265f;
        if (ImGui::DragFloat(tr("particle_editor.direction_min"), &dir_min_deg, 1.0f, -180.0f, 180.0f)) {
            emitter_->direction_min = dir_min_deg * 3.14159265f / 180.0f;
        }
        if (ImGui::DragFloat(tr("particle_editor.direction_max"), &dir_max_deg, 1.0f, -180.0f, 180.0f)) {
            emitter_->direction_max = dir_max_deg * 3.14159265f / 180.0f;
        }

        float acc[2] = { emitter_->acceleration.x, emitter_->acceleration.y };
        if (ImGui::DragFloat2(tr("particle_editor.acceleration"), acc, 1.0f)) {
            emitter_->acceleration = math::Vector2f(acc[0], acc[1]);
        }
    }
}

void ParticleEditorWindow::draw_appearance() {
    if (ImGui::CollapsingHeader(tr("particle_editor.appearance"), ImGuiTreeNodeFlags_DefaultOpen)) {
        float start_c[4] = { emitter_->start_color.r, emitter_->start_color.g,
                             emitter_->start_color.b, emitter_->start_color.a };
        if (ImGui::ColorEdit4(tr("particle_editor.start_color"), start_c)) {
            emitter_->start_color = render::Color(start_c[0], start_c[1], start_c[2], start_c[3]);
        }

        float end_c[4] = { emitter_->end_color.r, emitter_->end_color.g,
                           emitter_->end_color.b, emitter_->end_color.a };
        if (ImGui::ColorEdit4(tr("particle_editor.end_color"), end_c)) {
            emitter_->end_color = render::Color(end_c[0], end_c[1], end_c[2], end_c[3]);
        }

        ImGui::DragFloat(tr("particle_editor.start_size"), &emitter_->start_size, 0.5f, 0.0f, 1000.0f);
        ImGui::DragFloat(tr("particle_editor.end_size"), &emitter_->end_size, 0.5f, 0.0f, 1000.0f);

        ImGui::DragFloat(tr("particle_editor.rotation_min"), &emitter_->rotation_min, 1.0f, -360.0f, 360.0f);
        ImGui::DragFloat(tr("particle_editor.rotation_max"), &emitter_->rotation_max, 1.0f, -360.0f, 360.0f);
        ImGui::DragFloat(tr("particle_editor.angular_velocity_min"), &emitter_->angular_velocity_min, 1.0f, -3600.0f, 3600.0f);
        ImGui::DragFloat(tr("particle_editor.angular_velocity_max"), &emitter_->angular_velocity_max, 1.0f, -3600.0f, 3600.0f);

        char buf[256] = {};
        std::strncpy(buf, emitter_->texture_path.c_str(), sizeof(buf) - 1);
        if (ImGui::InputText(tr("particle_editor.texture"), buf, sizeof(buf))) {
            emitter_->texture_path = buf;
        }
    }
}

void ParticleEditorWindow::draw_preview() {
    ImGui::Text("%s", tr("particle_editor.preview"));

    const ImVec2 canvas_p0 = ImGui::GetCursorScreenPos();
    const ImVec2 canvas_sz(280.0f, 120.0f);
    const ImVec2 canvas_p1(canvas_p0.x + canvas_sz.x, canvas_p0.y + canvas_sz.y);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(canvas_p0, canvas_p1, IM_COL32(30, 30, 30, 255));

    // 颜色曲线
    for (int i = 0; i <= 64; ++i) {
        float t = static_cast<float>(i) / 64.0f;
        render::Color c = lerp_color(emitter_->start_color, emitter_->end_color, t);
        float x = canvas_p0.x + t * canvas_sz.x;
        draw->AddLine(ImVec2(x, canvas_p0.y), ImVec2(x, canvas_p0.y + canvas_sz.y * 0.5f),
                      IM_COL32(static_cast<int>(c.r * 255), static_cast<int>(c.g * 255),
                               static_cast<int>(c.b * 255), 255), 4.0f);
    }

    // 尺寸曲线
    float s0 = emitter_->start_size;
    float s1 = emitter_->end_size;
    for (int i = 0; i < 64; ++i) {
        float t0 = static_cast<float>(i) / 64.0f;
        float t1 = static_cast<float>(i + 1) / 64.0f;
        float size0 = s0 + (s1 - s0) * t0;
        float size1 = s0 + (s1 - s0) * t1;
        float y0 = canvas_p0.y + canvas_sz.y * 0.5f + (1.0f - size0 / std::max(s0 + s1, 1.0f)) * canvas_sz.y * 0.5f;
        float y1 = canvas_p0.y + canvas_sz.y * 0.5f + (1.0f - size1 / std::max(s0 + s1, 1.0f)) * canvas_sz.y * 0.5f;
        draw->AddLine(ImVec2(canvas_p0.x + t0 * canvas_sz.x, y0),
                      ImVec2(canvas_p0.x + t1 * canvas_sz.x, y1),
                      IM_COL32(200, 200, 200, 255), 2.0f);
    }

    ImGui::Dummy(canvas_sz);
}

void ParticleEditorWindow::draw_actions() {
    if (ImGui::Button(tr("particle_editor.burst"))) {
        emitter_->burst();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("particle_editor.clear"))) {
        emitter_->clear();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s: %d", tr("particle_editor.active_count"), emitter_->active_count());
}

} // namespace gryce_engine::editor
