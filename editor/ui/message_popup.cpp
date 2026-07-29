#include "message_popup.h"

#include <imgui.h>

#include "../localization/localization.h"

namespace gryce_engine::editor {

MessagePopup& MessagePopup::instance() {
    static MessagePopup popup;
    return popup;
}

void MessagePopup::warn(const std::string& message) {
    queue_.push_back({"common.warning", message});
    open_requested_ = true;
}

void MessagePopup::error(const std::string& message) {
    queue_.push_back({"common.error", message});
    open_requested_ = true;
}

void MessagePopup::draw() {
    if (queue_.empty()) return;

    const std::string title = std::string(tr(queue_.front().title_key)) + "###message_popup";
    if (open_requested_) {
        ImGui::OpenPopup(title.c_str());
        open_requested_ = false;
    }
    if (ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("%s", queue_.front().message.c_str());
        ImGui::Spacing();
        if (ImGui::Button(tr("common.ok"), ImVec2(120.0f, 0.0f))) {
            queue_.erase(queue_.begin());
            // 队列还有消息时下一帧继续弹
            open_requested_ = !queue_.empty();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

} // namespace gryce_engine::editor
