#include "project_panel.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "resources/project.h"
#include "resources/resource_path.h"
#include "utils/glog/glog_lib.h"
#include "../asset/asset_database.h"
#include "../localization/localization.h"

namespace gryce_engine::editor {

namespace {

constexpr const char* PROJECT_FILE_PAYLOAD = "GRYCE_PROJECT_FILE";

// 网格单元常量（以 14px 字体为基准，运行时按当前字号缩放）
constexpr float k_base_font_size = 14.0f;
constexpr float k_min_item_width = 84.0f; // 最小列宽，实际列宽会随面板宽度动态计算
constexpr float k_item_h_gap     = 0.0f;
constexpr float k_top_margin     = 6.0f;
constexpr float k_icon_size      = 46.0f;
constexpr float k_text_gap       = 6.0f;
constexpr float k_bottom_margin  = 6.0f;
constexpr float k_row_gap        = 4.0f;

float ui_scale() {
    return ImGui::GetFontSize() / k_base_font_size;
}

std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string extension_of(const std::filesystem::path& path) {
    return to_lower(path.extension().string());
}

// 文件类型色标（Unity 风格：通用文档图标 + 右下角类型色标）
struct FileIcon {
    const char* badge_label;
    ImU32 badge_color;
};

FileIcon icon_for(const std::filesystem::path& path, bool is_directory) {
    if (is_directory) return {nullptr, IM_COL32(0, 0, 0, 0)};

    const std::string ext = extension_of(path);
    if (ext == ".gesc")       return {"S",  IM_COL32(70,  130, 220, 255)};
    if (ext == ".gimport")    return {"I",  IM_COL32(170, 160, 70,  255)};
    if (ext == ".obj" || ext == ".fbx" || ext == ".gltf" || ext == ".glb" ||
        ext == ".dae" || ext == ".ply" || ext == ".stl")
                                return {"M",  IM_COL32(90,  180, 120, 255)};
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" ||
        ext == ".tga" || ext == ".dds" || ext == ".ktx" || ext == ".hdr" || ext == ".exr")
                                return {"T",  IM_COL32(170, 100, 220, 255)};
    if (ext == ".gmat")       return {"A",  IM_COL32(230, 140, 70,  255)};
    if (ext == ".vert" || ext == ".frag" || ext == ".spv" || ext == ".glsl" || ext == ".hlsl")
                                return {"H",  IM_COL32(140, 140, 140, 255)};
    if (ext == ".ttf" || ext == ".otf")
                                return {"F",  IM_COL32(230, 100, 160, 255)};
    if (ext == ".gryce")      return {"P",  IM_COL32(60,  180, 200, 255)};
    if (ext == ".wav" || ext == ".mp3" || ext == ".ogg" || ext == ".flac")
                                return {"Au", IM_COL32(100, 170, 210, 255)};
    if (ext == ".cpp" || ext == ".h" || ext == ".hpp" || ext == ".c" || ext == ".py" || ext == ".cs")
                                return {"C",  IM_COL32(80,  150, 210, 255)};
    return {"?", IM_COL32(160, 160, 160, 255)};
}

void draw_folder_icon(ImVec2 pos, float size) {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    const float w = size * 1.15f;
    const float h = size * 0.85f;
    const float tab_w = w * 0.38f;
    const float tab_h = h * 0.18f;

    pos.x += (size - w) * 0.5f;
    pos.y += (size - h) * 0.5f;

    ImVec2 back_min(pos.x, pos.y + tab_h * 0.5f);
    ImVec2 back_max(pos.x + w, pos.y + h);
    draw_list->AddRectFilled(back_min, back_max, IM_COL32(140, 140, 140, 255), 3.0f);

    ImVec2 front_min(pos.x, pos.y + tab_h * 1.2f);
    ImVec2 front_max(pos.x + w, pos.y + h);
    draw_list->AddRectFilled(front_min, front_max, IM_COL32(180, 180, 180, 255), 3.0f);

    ImVec2 tab_min(pos.x, pos.y);
    ImVec2 tab_max(pos.x + tab_w, pos.y + tab_h * 1.5f);
    draw_list->AddRectFilled(tab_min, tab_max, IM_COL32(170, 170, 170, 255), 2.0f);
}

void draw_document_icon(ImVec2 pos, float size, const FileIcon& icon) {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    const float w = size * 0.85f;
    const float h = size * 1.05f;
    pos.x += (size - w) * 0.5f;
    pos.y += (size - h) * 0.5f;

    ImVec2 body_min(pos.x, pos.y);
    ImVec2 body_max(pos.x + w, pos.y + h);
    ImU32 page_color = IM_COL32(220, 220, 220, 255);
    ImU32 fold_color = IM_COL32(190, 190, 190, 255);
    draw_list->AddRectFilled(body_min, body_max, page_color, 3.0f);

    float fold = w * 0.28f;
    ImVec2 p1(body_max.x - fold, body_min.y);
    ImVec2 p2(body_max.x, body_min.y);
    ImVec2 p3(body_max.x, body_min.y + fold);
    draw_list->AddTriangleFilled(p1, p2, p3, fold_color);

    if (icon.badge_label) {
        const float badge_h = size * 0.32f;
        const float badge_w = std::max(badge_h, ImGui::CalcTextSize(icon.badge_label).x + 6.0f);
        ImVec2 badge_min(body_max.x - badge_w - 2.0f, body_max.y - badge_h - 2.0f);
        ImVec2 badge_max(body_max.x - 2.0f, body_max.y - 2.0f);
        draw_list->AddRectFilled(badge_min, badge_max, icon.badge_color, 3.0f);

        ImVec2 text_size = ImGui::CalcTextSize(icon.badge_label);
        ImVec2 text_pos(badge_min.x + (badge_w - text_size.x) * 0.5f,
                        badge_min.y + (badge_h - text_size.y) * 0.5f);
        draw_list->AddText(text_pos, IM_COL32(255, 255, 255, 255), icon.badge_label);
    }
}

// 将文本按 max_width 拆分成多行，不限制行数
std::vector<std::string> wrap_text(const char* text, float max_width) {
    std::vector<std::string> lines;
    const char* p = text;
    const char* end = text + strlen(text);

    while (p < end) {
        int low = 0;
        int high = static_cast<int>(end - p);
        int best = 0;
        while (low <= high) {
            int mid = (low + high) / 2;
            float w = ImGui::CalcTextSize(p, p + mid).x;
            if (w <= max_width) {
                best = mid;
                low = mid + 1;
            } else {
                high = mid - 1;
            }
        }
        if (best == 0) best = 1;
        lines.emplace_back(p, p + best);
        p += best;
    }
    return lines;
}

} // namespace

FileExplorerPanel::FileExplorerPanel() : EditorPanel("File Explorer", "panel.file_explorer") {
    current_dir_ = resources::Project::instance().root();
    if (current_dir_.empty()) {
        current_dir_ = std::filesystem::current_path();
    }
}

std::string FileExplorerPanel::to_res_path(const std::filesystem::path& absolute) const {
    return resources::ResourcePath::make_relative(absolute.string(), resources::Project::instance().root());
}

void FileExplorerPanel::navigate_to(const std::filesystem::path& path) {
    if (std::filesystem::is_directory(path)) {
        current_dir_ = path;
    }
}

void FileExplorerPanel::draw_path_bar() {
    const std::string root = resources::Project::instance().root();
    std::string rel = current_dir_.string();
    if (!root.empty() && rel.size() >= root.size() && rel.compare(0, root.size(), root) == 0) {
        rel = rel.substr(root.size());
        if (!rel.empty() && (rel.front() == '/' || rel.front() == '\\')) {
            rel = rel.substr(1);
        }
    }
    if (rel.empty()) rel = tr("file_explorer.root");

    ImGui::Text(tr("file_explorer.assets_path"), rel.c_str());

    ImGui::SameLine();
    const bool can_go_up = !current_dir_.empty() && current_dir_ != std::filesystem::path(root);
    ImGui::BeginDisabled(!can_go_up);
    if (ImGui::Button(tr("file_explorer.up"))) {
        std::filesystem::path parent = current_dir_.parent_path();
        if (!parent.empty()) {
            current_dir_ = parent;
        }
    }
    ImGui::EndDisabled();
}

void FileExplorerPanel::draw_grid_item(const GridItem& item, ImVec2 pos, float item_width, float scale) {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const FileIcon icon = icon_for(item.entry.path(), item.is_dir);

    const float icon_size      = k_icon_size * scale;
    const float top_margin     = k_top_margin * scale;
    const float text_gap       = k_text_gap * scale;
    const float text_h_padding = 8.0f * scale;

    ImGui::PushID(item.name.c_str());
    ImGui::SetCursorScreenPos(pos);
    ImGui::InvisibleButton("##item", ImVec2(item_width, item.height));

    if (ImGui::IsItemHovered()) {
        draw_list->AddRectFilled(pos, ImVec2(pos.x + item_width, pos.y + item.height),
                                 ImGui::GetColorU32(ImGuiCol_HeaderHovered), 4.0f * scale);
    }

    // 图标
    ImVec2 icon_pos(pos.x + (item_width - icon_size) * 0.5f, pos.y + top_margin);
    if (item.is_dir) {
        draw_folder_icon(icon_pos, icon_size);
    } else {
        draw_document_icon(icon_pos, icon_size, icon);
    }

    // 文件名（按计算好的行换行，不限制行数）
    const float line_height = ImGui::GetFontSize();
    const float text_max_w = item_width - text_h_padding;
    float text_y = pos.y + top_margin + icon_size + text_gap;
    for (const auto& line : item.lines) {
        ImVec2 size = ImGui::CalcTextSize(line.c_str());
        draw_list->AddText(ImVec2(pos.x + text_h_padding * 0.5f + (text_max_w - size.x) * 0.5f, text_y),
                           ImGui::GetColorU32(ImGuiCol_Text), line.c_str());
        text_y += line_height;
    }

    // 交互
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        if (item.is_dir) {
            navigate_to(item.entry.path());
        } else if (on_activate_file) {
            on_activate_file(to_res_path(item.entry.path()));
        }
    }

    if (!item.is_dir && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        std::string guid = AssetDatabase::instance().guid_for_path(item.entry.path());
        std::string type = AssetDatabase::instance().infer_type(item.entry.path());
        ImGui::BeginTooltip();
        ImGui::Text("Type: %s", type.c_str());
        if (!guid.empty()) {
            ImGui::Text("GUID: %s", guid.c_str());
        }
        ImGui::EndTooltip();
    }

    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
        const std::string res_path = to_res_path(item.entry.path());
        ImGui::SetDragDropPayload(PROJECT_FILE_PAYLOAD, res_path.data(), res_path.size() + 1);

        ImVec2 tip_min = ImGui::GetCursorScreenPos();
        if (item.is_dir) {
            draw_folder_icon(tip_min, icon_size);
        } else {
            draw_document_icon(tip_min, icon_size, icon);
        }
        ImGui::Dummy(ImVec2(icon_size, icon_size));
        ImGui::SameLine();
        ImGui::TextUnformatted(item.name.c_str());

        ImGui::EndDragDropSource();
    }

    ImGui::PopID();
}

void FileExplorerPanel::on_imgui() {
    draw_path_bar();
    ImGui::Separator();

    if (!std::filesystem::is_directory(current_dir_)) {
        ImGui::TextDisabled("%s", tr("file_explorer.root_invalid"));
        return;
    }

    const float scale = ui_scale();

    // 横向网格布局：根据面板宽度动态计算列数和格子宽度
    ImVec2 start_pos = ImGui::GetCursorScreenPos();
    const float avail_width = ImGui::GetContentRegionAvail().x;
    const float min_item_width = k_min_item_width * scale;
    const float item_h_gap     = k_item_h_gap * scale;
    const float top_margin     = k_top_margin * scale;
    const float icon_size      = k_icon_size * scale;
    const float text_gap       = k_text_gap * scale;
    const float bottom_margin  = k_bottom_margin * scale;
    const float row_gap        = k_row_gap * scale;
    const float text_h_padding = 8.0f * scale;

    const int columns = std::max(1, static_cast<int>(avail_width / min_item_width));
    const float item_width = std::floor((avail_width - (columns - 1) * item_h_gap) / columns);

    std::vector<GridItem> items;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(current_dir_, ec)) {
        GridItem item;
        item.entry = entry;
        item.is_dir = entry.is_directory(ec);
        item.name = entry.path().filename().string();
        item.lines = wrap_text(item.name.c_str(), item_width - text_h_padding);
        const float text_h = static_cast<float>(item.lines.size()) * ImGui::GetFontSize();
        item.height = top_margin + icon_size + text_gap + text_h + bottom_margin;
        items.push_back(std::move(item));
    }

    auto sort_by_name = [](const GridItem& a, const GridItem& b) {
        return a.name < b.name;
    };
    // 文件夹排在前面，同类按名称排序
    std::partition(items.begin(), items.end(), [](const GridItem& i) { return i.is_dir; });
    auto dir_end = std::partition_point(items.begin(), items.end(), [](const GridItem& i) { return i.is_dir; });
    std::sort(items.begin(), dir_end, sort_by_name);
    std::sort(dir_end, items.end(), sort_by_name);

    const float row_right = start_pos.x + avail_width;
    float x = start_pos.x;
    float y = start_pos.y;
    float row_max_h = 0.0f;
    bool first_in_row = true;

    for (const auto& item : items) {
        if (!first_in_row && x + item_width > row_right) {
            x = start_pos.x;
            y += row_max_h + row_gap;
            row_max_h = 0.0f;
            first_in_row = true;
        }

        draw_grid_item(item, ImVec2(x, y), item_width, scale);
        x += item_width + item_h_gap;
        row_max_h = std::max(row_max_h, item.height);
        first_in_row = false;
    }
}

} // namespace gryce_engine::editor
