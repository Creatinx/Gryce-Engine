#include "animation_editor_window.h"

#include <imgui.h>

#include "../localization/localization.h"
#include "assets/asset_manager.h"
#include "assets/skinned_mesh_data.h"
#include "scene/entity.h"

namespace gryce_engine::editor {

void AnimationEditorWindow::open(scene::Entity* entity) {
    entity_ = entity;
    open_ = true;
}

void AnimationEditorWindow::draw() {
    if (!open_ || !entity_) return;

    auto* smr = entity_->get_component<components::SkinnedMeshRenderer>();
    if (!smr) {
        open_ = false;
        return;
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(420.0f, 360.0f), ImGuiCond_Appearing);

    bool still_open = true;
    if (ImGui::Begin(tr("animation_editor.title"), &still_open, ImGuiWindowFlags_NoDocking)) {
        ImGui::Text("%s: %s", tr("common.name"), entity_->name().c_str());
        ImGui::Separator();

        draw_clip_list(smr);
        ImGui::Separator();
        draw_playback_controls(smr);
    }
    ImGui::End();

    if (!still_open) {
        open_ = false;
        entity_ = nullptr;
    }
}

void AnimationEditorWindow::draw_clip_list(components::SkinnedMeshRenderer* smr) {
    ImGui::Text("%s", tr("animation_editor.clips"));

    const auto* model = smr->model().get();
    if (!model || model->animations.empty()) {
        ImGui::TextDisabled("%s", tr("animation_editor.no_clips"));
        return;
    }

    const std::string current = smr->clip_name;
    for (const auto& clip : model->animations) {
        bool selected = (current == clip.name);
        if (ImGui::Selectable(clip.name.empty() ? "(unnamed)" : clip.name.c_str(), selected)) {
            smr->clip_name = clip.name;
            smr->time = 0.0f;
        }
    }
}

void AnimationEditorWindow::draw_playback_controls(components::SkinnedMeshRenderer* smr) {
    const auto* model = smr->model().get();
    const float duration = model ? smr->resolve_clip()->duration : 0.0f;

    if (ImGui::Button(smr->playing ? tr("animation_editor.pause") : tr("animation_editor.play"))) {
        smr->playing = !smr->playing;
    }
    ImGui::SameLine();
    ImGui::Checkbox(tr("animation_editor.loop"), &smr->loop);
    ImGui::DragFloat(tr("animation_editor.speed"), &smr->speed, 0.01f, -5.0f, 5.0f);

    if (duration > 1e-6f) {
        float t = smr->time;
        if (ImGui::SliderFloat(tr("animation_editor.time"), &t, 0.0f, duration, "%.3f s")) {
            smr->time = t;
        }
        ImGui::ProgressBar(t / duration, ImVec2(-1.0f, 0.0f), std::format("{:.2f} / {:.2f} s", t, duration).c_str());
    } else {
        ImGui::TextDisabled("%s", tr("animation_editor.no_duration"));
    }
}

} // namespace gryce_engine::editor
