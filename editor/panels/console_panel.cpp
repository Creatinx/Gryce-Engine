#include "console_panel.h"

#include <filesystem>
#include <format>
#include <string>
#include <system_error>

#ifdef _WIN32
    #include <windows.h>
    #include <shellapi.h>
#endif

#include "../localization/localization.h"

namespace gryce_engine::editor {

namespace {

void open_source_location(const std::string& file, int line) {
    if (file.empty() || line <= 0) return;
#ifdef _WIN32
    std::error_code ec;
    const auto normalized = std::filesystem::canonical(file, ec);
    const std::string path = ec ? file : normalized.string();
    const std::string url = std::format("vscode://file/{}:{}", path, line);
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    (void)file;
    (void)line;
#endif
}

ImVec4 level_color(utils::LogLevel level) {
    const bool light_theme = Localization::instance().is_light_theme();
    if (light_theme) {
        switch (level) {
            case utils::LogLevel::Trace: return ImVec4(0.35f, 0.35f, 0.35f, 1.0f);
            case utils::LogLevel::Debug: return ImVec4(0.00f, 0.00f, 0.00f, 1.0f);
            case utils::LogLevel::Info:  return ImVec4(0.10f, 0.10f, 0.10f, 1.0f);
            case utils::LogLevel::Warn:  return ImVec4(0.60f, 0.40f, 0.00f, 1.0f);
            case utils::LogLevel::Error: return ImVec4(0.80f, 0.20f, 0.20f, 1.0f);
            case utils::LogLevel::Fatal: return ImVec4(0.90f, 0.10f, 0.10f, 1.0f);
        }
    } else {
        switch (level) {
            case utils::LogLevel::Trace: return ImVec4(0.55f, 0.55f, 0.55f, 1.0f);
            case utils::LogLevel::Debug: return ImVec4(1.00f, 1.00f, 1.00f, 1.0f);
            case utils::LogLevel::Info:  return ImVec4(0.90f, 0.90f, 0.90f, 1.0f);
            case utils::LogLevel::Warn:  return ImVec4(0.95f, 0.80f, 0.30f, 1.0f);
            case utils::LogLevel::Error: return ImVec4(0.95f, 0.35f, 0.35f, 1.0f);
            case utils::LogLevel::Fatal: return ImVec4(1.00f, 0.20f, 0.20f, 1.0f);
        }
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
    ImGui::Checkbox(tr("console.trace"), &show_trace_);
    ImGui::SameLine();
    ImGui::Checkbox(tr("console.debug"), &show_debug_);
    ImGui::SameLine();
    ImGui::Checkbox(tr("console.info"), &show_info_);
    ImGui::SameLine();
    ImGui::Checkbox(tr("console.warn"), &show_warn_);
    ImGui::SameLine();
    ImGui::Checkbox(tr("console.error"), &show_error_);
    ImGui::SameLine();
    ImGui::Checkbox(tr("console.auto_scroll"), &auto_scroll_);
    ImGui::SameLine();

    utils::MemoryLogSink* sink = utils::MemoryLogSink::from_glog();
    if (ImGui::Button(tr("console.clear")) && sink) {
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

            std::string label = std::format("[{}] [{}] {}", entry.timestamp,
                                            utils::MemoryLogSink::level_str(entry.level),
                                            entry.message);
            if (!entry.source_file.empty() && entry.source_line > 0) {
                label += std::format("  @ {}:{}", entry.source_file, entry.source_line);
            }

            ImGui::PushID(&entry);
            if (ImGui::Selectable(label.c_str(), false,
                                  ImGuiSelectableFlags_AllowDoubleClick)) {
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
                    ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    open_source_location(entry.source_file, entry.source_line);
                }
            }
            ImGui::PopID();
            ImGui::PopStyleColor();
        }
    } else {
        ImGui::TextDisabled("%s", tr("console.sink_missing"));
    }

    // 已贴底时跟随最新日志
    if (auto_scroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
}

} // namespace gryce_engine::editor
