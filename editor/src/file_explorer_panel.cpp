#include "file_explorer_panel.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include <imgui.h>

#include "i18n.h"

namespace gryce_engine::editor {

namespace fs = std::filesystem;

namespace {

const char* tr(const char* key) { return I18n::instance().tr(key); }

// 简单的 Godot 风格图标：文件夹 / 文档（用 DrawList 绘制，无需字体图标）
void draw_folder_icon(ImDrawList* dl, ImVec2 pos, float size) {
    const float w = size;
    const float h = size * 0.88f;
    const ImU32 body = IM_COL32(0x5B, 0x7F, 0xA6, 255);
    const ImU32 body_dark = IM_COL32(0x4A, 0x68, 0x8B, 255);
    const ImU32 tab = IM_COL32(0x7A, 0x9C, 0xC4, 255);

    // 文件夹提手
    dl->AddRectFilled(ImVec2(pos.x, pos.y + h * 0.22f),
                      ImVec2(pos.x + w * 0.38f, pos.y + h * 0.42f), tab, 1.0f);
    // 文件夹主体
    dl->AddRectFilled(ImVec2(pos.x, pos.y + h * 0.30f),
                      ImVec2(pos.x + w, pos.y + h), body, 2.0f);
    // 底部阴影边
    dl->AddRectFilled(ImVec2(pos.x, pos.y + h * 0.84f),
                      ImVec2(pos.x + w, pos.y + h), body_dark, 2.0f);
}

void draw_file_icon(ImDrawList* dl, ImVec2 pos, float size, ImU32 accent) {
    const float w = size;
    const float h = size * 0.88f;
    const ImU32 page = IM_COL32(0xC8, 0xCC, 0xD2, 255);
    const ImU32 page_dark = IM_COL32(0xA8, 0xAC, 0xB4, 255);

    // 文档主体（右上角折角）
    dl->AddRectFilled(ImVec2(pos.x + 1.0f, pos.y),
                      ImVec2(pos.x + w - 3.0f, pos.y + h - 3.0f), page, 1.5f);
    // 折角
    dl->AddTriangleFilled(ImVec2(pos.x + w - 8.0f, pos.y + 1.0f),
                          ImVec2(pos.x + w - 3.0f, pos.y + 1.0f),
                          ImVec2(pos.x + w - 3.0f, pos.y + 6.0f),
                          page_dark);
    // 底部类型色条
    dl->AddRectFilled(ImVec2(pos.x + 1.0f, pos.y + h - 5.0f),
                      ImVec2(pos.x + w - 3.0f, pos.y + h - 3.0f), accent, 1.0f);
}

} // namespace

void FileExplorerPanel::set_root(const fs::path& root) {
    root_ = fs::absolute(root);
    if (current_.empty() || current_ == root_) {
        current_ = root_;
    }
}

void FileExplorerPanel::render() {
    render_as_tab(false);
}

// 作为底部面板 Tab（FileSystem + 历史记录），或者独立面板。
void FileExplorerPanel::render_as_tab(bool file_system_tab) {
    if (file_system_tab) {
        static int tab = 0;
        ImGui::Begin("FileSystem");
        if (ImGui::BeginTabBar("explorer_tabs")) {
            if (ImGui::BeginTabItem(tr("FileSystem"))) {
                tab = 0;
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(tr("History"))) {
                tab = 1;
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        if (tab != 0) {
            ImGui::TextDisabled(tr("History tab is not yet implemented"));
            ImGui::End();
            return;
        }
    } else {
        ImGui::Begin("Project");
    }

    if (root_.empty()) {
        ImGui::TextUnformatted(tr("Project root is not configured"));
        ImGui::End();
        return;
    }

    render_breadcrumbs();
    ImGui::Separator();
    render_list();
    ImGui::End();
}

FileExplorerPanel::EntryStyle FileExplorerPanel::classify(const fs::path& path) const {
    std::error_code ec;
    if (fs::is_directory(path, ec)) {
        return {EntryKind::Directory, 0.36f, 0.50f, 0.65f};
    }

    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" ||
        ext == ".tga" || ext == ".dds" || ext == ".ktx" || ext == ".hdr" ||
        ext == ".exr") {
        return {EntryKind::Image, 0.84f, 0.56f, 0.18f};
    }
    if (ext == ".wav" || ext == ".ogg" || ext == ".mp3" || ext == ".flac") {
        return {EntryKind::Audio, 0.35f, 0.72f, 0.42f};
    }
    if (ext == ".obj" || ext == ".gltf" || ext == ".glb" || ext == ".fbx" ||
        ext == ".dae" || ext == ".ply" || ext == ".stl") {
        return {EntryKind::Model3D, 0.18f, 0.52f, 0.86f};
    }
    if (ext == ".gesc") {
        return {EntryKind::Scene, 0.16f, 0.62f, 0.48f};
    }
    if (ext == ".gmat") {
        return {EntryKind::Material, 0.68f, 0.37f, 0.90f};
    }
    if (ext == ".lua" || ext == ".py") {
        return {EntryKind::Script, 0.22f, 0.47f, 0.91f};
    }
    if (ext == ".txt" || ext == ".md" || ext == ".json") {
        return {EntryKind::Text, 0.48f, 0.52f, 0.57f};
    }
    return {EntryKind::File, 0.45f, 0.48f, 0.52f};
}

void FileExplorerPanel::navigate_to(const fs::path& path) {
    std::error_code ec;
    if (fs::is_directory(path, ec)) {
        current_ = fs::absolute(path, ec);
    }
}

void FileExplorerPanel::navigate_up() {
    if (!current_.has_parent_path()) return;
    if (current_ == root_) return; // res:// 根不可再向上
    current_ = current_.parent_path();
}

bool FileExplorerPanel::is_hidden(const fs::path& path) const {
    const std::string name = path.filename().string();
    if (!name.empty() && name.front() == '.') return true;
    // 常见构建/IDE 输出目录，Godot 风格默认隐藏
    static const char* k_hidden_dirs[] = {
        "build", "build-msvc", "build_vs", "build_mingw_test",
        "cmake-build-debug", "cmake-build-release", "deps_cache",
        "__pycache__", "out", "x64",
    };
    for (const char* d : k_hidden_dirs) {
        if (name == d) return true;
    }
    return false;
}

std::string FileExplorerPanel::display_path() const {
    std::error_code ec;
    fs::path rel = fs::relative(current_, root_, ec);
    std::string out = "res://";
    if (!ec && !rel.empty() && rel != ".") {
        std::string rel_str = rel.generic_string();
        if (rel_str == "..") return out;
        out += rel_str;
    }
    return out;
}

void FileExplorerPanel::render_breadcrumbs() {
    if (current_ != root_) {
        if (ImGui::Button("..")) {
            navigate_up();
        }
        ImGui::SameLine();
    }

    ImGui::TextUnformatted("res://");

    std::error_code ec;
    fs::path rel = fs::relative(current_, root_, ec);
    std::vector<std::string> parts;
    if (!ec) {
        for (const auto& seg : rel) {
            parts.push_back(seg.string());
        }
    }

    for (size_t i = 0; i < parts.size(); ++i) {
        ImGui::SameLine();
        ImGui::TextUnformatted("/");
        ImGui::SameLine();
        const std::string label = parts[i] + "##crumb" + std::to_string(i);
        if (ImGui::SmallButton(label.c_str())) {
            fs::path target = root_;
            for (size_t j = 0; j <= i; ++j) {
                target /= parts[j];
            }
            navigate_to(target);
        }
    }
}

void FileExplorerPanel::render_list() {
    std::error_code ec;
    if (!fs::exists(current_, ec) || !fs::is_directory(current_, ec)) {
        ImGui::TextColored(ImVec4(0.8f, 0.35f, 0.30f, 1.0f),
                           tr("Directory not found: %s"),
                           current_.string().c_str());
        return;
    }

    std::vector<fs::directory_entry> entries;
    for (const auto& entry : fs::directory_iterator(current_, ec)) {
        if (ec) break;
        if (is_hidden(entry.path())) continue;
        entries.push_back(entry);
        ec.clear();
    }
    std::sort(entries.begin(), entries.end(),
              [](const fs::directory_entry& a, const fs::directory_entry& b) {
                  const bool ad = a.is_directory();
                  const bool bd = b.is_directory();
                  if (ad != bd) return ad > bd;
                  return a.path().filename().string() < b.path().filename().string();
              });

    if (entries.empty()) {
        ImGui::TextDisabled(tr("(empty)"));
        return;
    }

    ImGui::BeginChild("explorer_list");
    const float row_height = ImGui::GetTextLineHeight() + 8.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& path = entries[i].path();
        const bool is_dir = entries[i].is_directory(ec);
        ec.clear();
        const EntryStyle style = classify(path);

        ImGui::PushID(static_cast<int>(i));
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float avail_w = ImGui::GetContentRegionAvail().x;
        const bool is_selected = (path == selected_);

        ImGui::InvisibleButton("##row", ImVec2(avail_w, row_height));
        const bool hovered = ImGui::IsItemHovered();
        const bool clicked = ImGui::IsItemClicked();

        if (clicked) {
            selected_ = path;
        }
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && is_dir) {
            navigate_to(path);
            selected_.clear();
        }

        // 行背景（选中/悬停）
        if (is_selected) {
            dl->AddRectFilled(p0, ImVec2(p0.x + avail_w, p0.y + row_height),
                              IM_COL32(0x2A, 0x4A, 0x78, 200));
        } else if (hovered) {
            dl->AddRectFilled(p0, ImVec2(p0.x + avail_w, p0.y + row_height),
                              IM_COL32(0x3A, 0x3A, 0x3A, 180));
        }

        // 图标（左侧居中）
        const ImVec2 icon_pos(p0.x + 6.0f,
                              p0.y + (row_height - 18.0f) * 0.5f);
        if (is_dir) {
            draw_folder_icon(dl, icon_pos, 18.0f);
        } else {
            draw_file_icon(dl, icon_pos, 18.0f,
                           IM_COL32(static_cast<int>(style.r * 255.0f),
                                    static_cast<int>(style.g * 255.0f),
                                    static_cast<int>(style.b * 255.0f), 255));
        }

        // 名字
        const std::string name = path.filename().string();
        const ImVec2 text_pos(p0.x + 30.0f,
                              p0.y + (row_height - ImGui::GetTextLineHeight()) * 0.5f);
        const ImU32 text_color = is_dir
                                     ? IM_COL32(0xF0, 0xF0, 0xF0, 255)
                                     : IM_COL32(0xC8, 0xC8, 0xC8, 255);
        dl->AddText(text_pos, text_color, name.c_str());

        ImGui::PopID();
    }
    ImGui::EndChild();
}

} // namespace gryce_engine::editor
