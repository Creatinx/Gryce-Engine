#include "console_panel.h"

namespace gryce_engine::editor {

namespace {

ImVec4 level_color(utils::LogLevel level) {
    switch (level) {
        case utils::LogLevel::Trace: return ImVec4(0.55f, 0.55f, 0.55f, 1.0f);
        case utils::LogLevel::Debug: return ImVec4(1.00f, 1.00f, 1.00f, 1.0f);
        case utils::LogLevel::Info:  return ImVec4(0.90f, 0.90f, 0.90f, 1.0f);
        case utils::LogLevel::Warn:  return ImVec4(0.95f, 0.80f, 0.30f, 1.0f);
        case utils::LogLevel::Error: return ImVec4(0.95f, 0.35f, 0.35f, 1.0f);
        case utils::LogLevel::Fatal: return ImVec4(1.00f, 0.20f, 0.20f, 1.0f);
    }
    return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
}

} // namespace

bool ConsolePanel::passes_filter(utils::LogLevel level) const {
    switch (level) {
        case utils::LogLevel::Trace: return show_trace_;
        case utils::LogLevel::Debug: return show_debug_;
        case utils::LogLevel::Info:  return show_info_;
        case utils::LogLevel::Warn:  return show_warn_;
        case utils::LogLevel::Error:
        case utils::LogLevel::Fatal: return show_error_;
    }
    return true;
}

void ConsolePanel::on_imgui() {
    // 工具行：级别过滤 + 自动滚动 + 清空
    ImGui::Checkbox("Trace", &show_trace_);
    ImGui::SameLine();
    ImGui::Checkbox("Debug", &show_debug_);
    ImGui::SameLine();
    ImGui::Checkbox("Info", &show_info_);
    ImGui::SameLine();
    ImGui::Checkbox("Warn", &show_warn_);
    ImGui::SameLine();
    ImGui::Checkbox("Error", &show_error_);
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &auto_scroll_);
    ImGui::SameLine();

    utils::MemoryLogSink* sink = utils::MemoryLogSink::from_glog();
    if (ImGui::Button("Clear") && sink) {
        sink->clear();
    }
    ImGui::Separator();

    ImGui::BeginChild("##console_scroll", ImVec2(0.0f, 0.0f), false,
                      ImGuiWindowFlags_HorizontalScrollbar);
    if (sink) {
        const auto entries = sink->snapshot();
        for (const auto& entry : entries) {
            if (!passes_filter(entry.level)) continue;
            ImGui::PushStyleColor(ImGuiCol_Text, level_color(entry.level));
            ImGui::TextUnformatted(
                std::format("[{}] [{}] {}", entry.timestamp,
                            utils::MemoryLogSink::level_str(entry.level), entry.message)
                    .c_str());
            ImGui::PopStyleColor();
        }
    } else {
        ImGui::TextDisabled("MemoryLogSink not installed (editor installs it at startup)");
    }

    // 已贴底时跟随最新日志
    if (auto_scroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
}

} // namespace gryce_engine::editor
