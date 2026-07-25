#include "game_view_panel.h"

#include "render/imgui_backend.h"
#include "render/render_pipeline.h"
#include "render/texture.h"
#include "../localization/localization.h"

namespace gryce_engine::editor {

GameViewPanel::GameViewPanel() : EditorPanel("Game", "panel.game") {}

void GameViewPanel::on_imgui() {
    size_ = ImGui::GetContentRegionAvail();

    if (!pipeline_ || !backend_ || size_.x < 1.0f || size_.y < 1.0f) {
        ImGui::TextDisabled("%s", tr("game_view.no_output"));
        return;
    }

    render::ITexture* tex = pipeline_->viewport_color_texture();
    if (!tex) {
        ImGui::TextDisabled("%s", tr("game_view.no_output"));
        return;
    }

    const uint64_t tex_id = backend_->imgui_texture_id(tex);
    if (tex_id == 0) {
        ImGui::TextDisabled("%s", tr("game_view.no_output"));
        return;
    }

    // OpenGL FBO 原点在左下，ImGui 图像原点在左上，翻转 V 轴；
    // Vulkan 通过负 viewport height 已把纹理方向调整为顶向下，不需要翻转。
    const bool is_vulkan = backend_ && backend_->is_vulkan();
    const ImVec2 uv0 = is_vulkan ? ImVec2(0.0f, 0.0f) : ImVec2(0.0f, 1.0f);
    const ImVec2 uv1 = is_vulkan ? ImVec2(1.0f, 1.0f) : ImVec2(1.0f, 0.0f);
    ImGui::Image(ImTextureRef(static_cast<ImTextureID>(tex_id)), size_, uv0, uv1);
}

} // namespace gryce_engine::editor
