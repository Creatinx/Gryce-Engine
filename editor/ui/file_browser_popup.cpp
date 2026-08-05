#include "file_browser_popup.h"

#include <algorithm>
#include <cstring>
#include <format>

#include <imgui.h>

#include "resources/project.h"
#include "resources/resource_path.h"
#include "../assets_manager/asset_database.h"
#include "../localization/localization.h"
#include "../platform_utils.h"
#include "message_popup.h"

namespace gryce_engine::editor {

FileBrowserPopup& FileBrowserPopup::instance() {
    static FileBrowserPopup popup;
    return popup;
}

void FileBrowserPopup::open_for(char* buf, size_t buf_size) {
    target_buf_ = buf;
    target_size_ = buf_size;
    selected_ = -1;

    root_str_ = resources::Project::instance().root();
    root_ = utf8_path(root_str_);
    if (root_str_.empty() || !std::filesystem::is_directory(root_)) {
        root_ = std::filesystem::current_path();
        root_str_ = root_.string();
    }
    current_dir_ = root_;

    // 输入框已有合法 res:/ 路径时，定位到该文件所在目录并预选中
    if (buf && buf[0] != '\0' && resources::ResourcePath::is_resource_path(buf)) {
        const std::filesystem::path resolved = utf8_path(resources::ResourcePath::resolve(buf));
        std::error_code ec;
        if (std::filesystem::exists(resolved, ec)) {
            current_dir_ = std::filesystem::is_directory(resolved, ec) ? resolved : resolved.parent_path();
        } else {
            // 引用的文件不存在：弹警告提醒用户（不只打日志）
            MessagePopup::instance().warn(
                std::vformat(tr("file_browser.path_not_found"), std::make_format_args(buf)));
        }
    }

    refresh_entries();

    // 预选与当前值同名的条目
    if (buf && buf[0] != '\0' && resources::ResourcePath::is_resource_path(buf)) {
        const std::string want = path_to_utf8(utf8_path(resources::ResourcePath::resolve(buf)).filename());
        for (size_t i = 0; i < entries_.size(); ++i) {
            if (entries_[i].name == want) {
                selected_ = static_cast<int>(i);
                break;
            }
        }
    }

    open_requested_ = true;
}

void FileBrowserPopup::refresh_entries() {
    entries_.clear();

    std::error_code ec;
    for (const auto& dir_entry : std::filesystem::directory_iterator(current_dir_, ec)) {
        Entry e;
        e.path = dir_entry.path();
        e.name = path_to_utf8(dir_entry.path().filename());
        e.is_dir = dir_entry.is_directory(ec);
        if (e.is_dir) {
            e.type = tr("file_browser.folder");
        } else {
            e.size = dir_entry.file_size(ec);
            if (ec) {
                e.size = 0;
                ec.clear();
            }
            e.type = AssetDatabase::infer_type(dir_entry.path());
        }
        e.modified = format_file_time(dir_entry.path());
        entries_.push_back(std::move(e));
    }

    // 文件夹在前，同类按名称排序（与 Project 面板一致）
    auto by_name = [](const Entry& a, const Entry& b) { return a.name < b.name; };
    std::partition(entries_.begin(), entries_.end(), [](const Entry& e) { return e.is_dir; });
    auto dir_end = std::partition_point(entries_.begin(), entries_.end(),
                                        [](const Entry& e) { return e.is_dir; });
    std::sort(entries_.begin(), dir_end, by_name);
    std::sort(dir_end, entries_.end(), by_name);

    selected_ = -1;
}

std::string FileBrowserPopup::current_res_dir() const {
    std::string rel = resources::ResourcePath::make_relative(path_to_utf8(current_dir_), root_str_);
    if (rel.empty() || rel == ".") return "res:/";
    return rel;
}

void FileBrowserPopup::confirm_selection() {
    if (selected_ < 0 || selected_ >= static_cast<int>(entries_.size())) return;
    const Entry& e = entries_[static_cast<size_t>(selected_)];
    if (e.is_dir) return;

    const std::string res_path = resources::ResourcePath::make_relative(
        path_to_utf8(e.path), root_str_);
    std::strncpy(target_buf_, res_path.c_str(), target_size_ - 1);
    target_buf_[target_size_ - 1] = '\0';
    target_buf_ = nullptr;
    ImGui::CloseCurrentPopup();
}

float FileBrowserPopup::browse_button_width() {
    // 与 ImGui 实际按钮渲染宽度一致（文本 + 两侧 FramePadding），再加安全余量
    return ImGui::CalcTextSize(tr("file_browser.browse")).x +
           ImGui::GetStyle().FramePadding.x * 2.0f + 4.0f;
}

bool FileBrowserPopup::browse_button(const char* id, char* buf, size_t buf_size) {
    ImGui::PushID(id);
    bool changed = false;

    if (ImGui::Button(tr("file_browser.browse"))) {
        open_for(buf, buf_size);
    }

    // 弹窗只由发起它的那个输入框绘制（target_buf_ 指向谁的缓冲区就由谁画）
    if (target_buf_ == buf) {
        if (open_requested_) {
            ImGui::OpenPopup(tr("file_browser.title"));
            open_requested_ = false;
        }
        changed = draw_popup();
    }

    ImGui::PopID();
    return changed;
}

bool FileBrowserPopup::draw_popup() {
    bool changed = false;

    ImGui::SetNextWindowSize(ImVec2(640.0f, 420.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(tr("file_browser.title"), nullptr, 0)) {
        return false;
    }

    // 顶部：上一级 + 当前路径
    const bool can_go_up = current_dir_ != root_;
    ImGui::BeginDisabled(!can_go_up);
    if (ImGui::Button(tr("file_explorer.up"))) {
        current_dir_ = current_dir_.parent_path();
        refresh_entries();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextUnformatted(current_res_dir().c_str());
    ImGui::Separator();

    // 详细列表：名称 / 类型 / 大小 / 修改时间
    const float footer_h = ImGui::GetFrameHeightWithSpacing() * 2.0f;
    if (ImGui::BeginTable("##fb_entries", 4,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                          ImVec2(0.0f, -footer_h))) {
        ImGui::TableSetupColumn(tr("file_browser.col_name"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(tr("file_browser.col_type"), ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn(tr("file_browser.col_size"), ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn(tr("file_browser.col_modified"), ImGuiTableColumnFlags_WidthFixed, 140.0f);
        ImGui::TableHeadersRow();

        std::filesystem::path pending_nav;
        bool confirm_selected = false;
        for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
            const Entry& e = entries_[static_cast<size_t>(i)];
            ImGui::PushID(i);
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            if (ImGui::Selectable(e.name.c_str(), selected_ == i,
                                  ImGuiSelectableFlags_SpanAllColumns |
                                  ImGuiSelectableFlags_AllowDoubleClick)) {
                selected_ = i;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    if (e.is_dir) {
                        pending_nav = e.path; // 延迟到循环外跳转，避免遍历中修改 entries_
                    } else {
                        confirm_selected = true;
                    }
                }
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(e.type.c_str());
            ImGui::TableNextColumn();
            if (!e.is_dir) {
                ImGui::TextUnformatted(format_file_size(e.size).c_str());
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(e.modified.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();

        if (!pending_nav.empty()) {
            current_dir_ = pending_nav;
            refresh_entries();
        } else if (confirm_selected) {
            confirm_selection();
            changed = true;
        }
    }

    // 底部：选中项 + 确认 / 取消
    const bool has_file = selected_ >= 0 && selected_ < static_cast<int>(entries_.size()) &&
                          !entries_[static_cast<size_t>(selected_)].is_dir;
    ImGui::Text("%s: %s", tr("file_browser.selected"),
                has_file ? entries_[static_cast<size_t>(selected_)].name.c_str() : "-");

    ImGui::BeginDisabled(!has_file);
    if (ImGui::Button(tr("common.ok"), ImVec2(120.0f, 0.0f)) && has_file) {
        confirm_selection();
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(tr("common.cancel"), ImVec2(120.0f, 0.0f))) {
        target_buf_ = nullptr;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
    return changed;
}

} // namespace gryce_engine::editor
