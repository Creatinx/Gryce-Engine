#include "console_panel.h"

#include <imgui.h>

#include "i18n.h"
#include "utils/glog/glog_lib.h"
#include "fluent_components.h"

namespace gryce_engine::editor {

namespace {
const char* tr(const char* key) { return I18n::instance().tr(key); }
} // namespace

void ConsolePanel::render() {
    ImGui::Begin("Output");

    // 日志级别下拉框
    const char* level_items[] = {
        tr("Error"), tr("Warn"), tr("Info"), tr("Debug")
    };
    const int level_values[] = {
        static_cast<int>(utils::LogLevel::Error),
        static_cast<int>(utils::LogLevel::Warn),
        static_cast<int>(utils::LogLevel::Info),
        static_cast<int>(utils::LogLevel::Debug)
    };
    int current_idx = 0;
    for (int i = 0; i < 4; ++i) {
        if (level_values[i] == static_cast<int>(min_display_level_)) {
            current_idx = i;
            break;
        }
    }

    ImGui::Text(tr("Level:"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    if (ImGui::Combo("##log_level", &current_idx, level_items, 4)) {
        min_display_level_ = static_cast<utils::LogLevel>(level_values[current_idx]);
        // 同步更新 GLog 的最小级别，确保日志被捕获
        utils::GLog::instance().set_min_level(min_display_level_);
    }

    ImGui::SameLine();
    FluentCheckbox auto_scroll(&auto_scroll_);
    auto_scroll.Draw(tr("Auto-scroll"), fluent_theme());

    ImGui::SameLine();
    ImGui::InputTextWithHint("##console_filter", tr("Filter..."), filter_,
                             sizeof(filter_));
    ImGui::SameLine();
    FluentButton clear_btn;
    if (clear_btn.Draw(tr("Clear"), fluent_theme())) {
        if (auto* sink = utils::MemoryLogSink::from_glog()) {
            sink->clear();
        }
    }

    ImGui::Separator();
    ImGui::BeginChild("console_scroll", ImVec2(0.0f, 0.0f), false,
                      ImGuiWindowFlags_HorizontalScrollbar);

    auto* sink = utils::MemoryLogSink::from_glog();
    if (!sink) {
        ImGui::TextDisabled(tr("Memory log sink is not installed"));
        ImGui::EndChild();
        ImGui::End();
        return;
    }

    const std::vector<utils::LogEntry> entries = sink->snapshot();
    const std::string filter = filter_ ? std::string(filter_) : std::string();

    for (const auto& entry : entries) {
        // 按最小显示级别过滤
        if (static_cast<int>(entry.level) < static_cast<int>(min_display_level_))
            continue;

        if (!filter.empty() &&
            entry.message.find(filter) == std::string::npos &&
            entry.source_file.find(filter) == std::string::npos) {
            continue;
        }

        ImVec4 color = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        if (entry.level == utils::LogLevel::Warn) {
            color = ImVec4(0.95f, 0.72f, 0.20f, 1.0f);
        } else if (entry.level == utils::LogLevel::Error ||
                   entry.level == utils::LogLevel::Fatal) {
            color = ImVec4(0.95f, 0.32f, 0.30f, 1.0f);
        } else if (entry.level == utils::LogLevel::Debug ||
                   entry.level == utils::LogLevel::Trace) {
            color = ImVec4(0.55f, 0.58f, 0.62f, 1.0f);
        }

        ImGui::TextColored(color, "[%s][%s] %s",
                           entry.timestamp.c_str(),
                           utils::MemoryLogSink::level_str(entry.level),
                           entry.message.c_str());
        if (ImGui::IsItemHovered() &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            ImGui::SetClipboardText(entry.message.c_str());
        }
    }

    if (auto_scroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f) {
        ImGui::SetScrollHereY(1.0f);
    }

    ImGui::EndChild();
    ImGui::End();
}

} // namespace gryce_engine::editor