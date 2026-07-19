#include "project_panel.h"

#include <algorithm>
#include <cctype>

#include "resources/project.h"
#include "resources/resource_path.h"
#include "utils/glog/glog_lib.h"
#include "../localization/localization.h"

namespace gryce_engine::editor {

namespace {

constexpr const char* PROJECT_FILE_PAYLOAD = "GRYCE_PROJECT_FILE";

std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string extension_of(const std::filesystem::path& path) {
    return to_lower(path.extension().string());
}

const char* icon_for(const std::filesystem::path& path, bool is_directory) {
    if (is_directory) return "[D]";

    const std::string ext = extension_of(path);
    if (ext == ".gesc") return "[S]";
    if (ext == ".obj" || ext == ".fbx" || ext == ".gltf" || ext == ".glb") return "[M]";
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" ||
        ext == ".tga" || ext == ".dds" || ext == ".ktx" || ext == ".hdr") return "[T]";
    if (ext == ".gmat") return "[A]";
    if (ext == ".vert" || ext == ".frag" || ext == ".spv") return "[H]";
    if (ext == ".ttf" || ext == ".otf") return "[F]";
    if (ext == ".gryce") return "[P]";
    return "[?]";
}

} // namespace

ProjectPanel::ProjectPanel() : EditorPanel("Project", "panel.project") {
    current_dir_ = resources::Project::instance().root();
    if (current_dir_.empty()) {
        current_dir_ = std::filesystem::current_path();
    }
}

std::string ProjectPanel::to_res_path(const std::filesystem::path& absolute) const {
    return resources::ResourcePath::make_relative(absolute.string(), resources::Project::instance().root());
}

void ProjectPanel::navigate_to(const std::filesystem::path& path) {
    if (std::filesystem::is_directory(path)) {
        current_dir_ = path;
    }
}

void ProjectPanel::draw_path_bar() {
    const std::string root = resources::Project::instance().root();
    std::string rel = current_dir_.string();
    if (!root.empty() && rel.size() >= root.size() && rel.compare(0, root.size(), root) == 0) {
        rel = rel.substr(root.size());
        if (!rel.empty() && (rel.front() == '/' || rel.front() == '\\')) {
            rel = rel.substr(1);
        }
    }
    if (rel.empty()) rel = tr("project.root");

    ImGui::Text(tr("project.assets_path"), rel.c_str());

    ImGui::SameLine();
    const bool can_go_up = !current_dir_.empty() && current_dir_ != std::filesystem::path(root);
    ImGui::BeginDisabled(!can_go_up);
    if (ImGui::Button(tr("project.up"))) {
        std::filesystem::path parent = current_dir_.parent_path();
        if (!parent.empty()) {
            current_dir_ = parent;
        }
    }
    ImGui::EndDisabled();
}

void ProjectPanel::draw_entry(const std::filesystem::directory_entry& entry) {
    const bool is_dir = entry.is_directory();
    const std::string name = entry.path().filename().string();
    const char* icon = icon_for(entry.path(), is_dir);

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf |
                               ImGuiTreeNodeFlags_SpanAvailWidth;
    if (is_dir) {
        flags |= ImGuiTreeNodeFlags_DefaultOpen;
    }

    const bool open = ImGui::TreeNodeEx(name.c_str(), flags, "%s %s", icon, name.c_str());
    if (open) {
        ImGui::TreePop();
    }

    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        if (is_dir) {
            navigate_to(entry.path());
        } else if (on_activate_file) {
            on_activate_file(to_res_path(entry.path()));
        }
    }

    // 拖拽源
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
        const std::string res_path = to_res_path(entry.path());
        ImGui::SetDragDropPayload(PROJECT_FILE_PAYLOAD, res_path.data(), res_path.size() + 1);
        ImGui::Text("%s %s", icon, name.c_str());
        ImGui::EndDragDropSource();
    }
}

void ProjectPanel::on_imgui() {
    draw_path_bar();
    ImGui::Separator();

    if (!std::filesystem::is_directory(current_dir_)) {
        ImGui::TextDisabled("%s", tr("project.root_invalid"));
        return;
    }

    std::vector<std::filesystem::directory_entry> dirs;
    std::vector<std::filesystem::directory_entry> files;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(current_dir_, ec)) {
        if (entry.is_directory(ec)) {
            dirs.push_back(entry);
        } else {
            files.push_back(entry);
        }
    }

    auto sort_by_name = [](const auto& a, const auto& b) {
        return a.path().filename().string() < b.path().filename().string();
    };
    std::sort(dirs.begin(), dirs.end(), sort_by_name);
    std::sort(files.begin(), files.end(), sort_by_name);

    for (const auto& entry : dirs) {
        draw_entry(entry);
    }
    for (const auto& entry : files) {
        draw_entry(entry);
    }
}

} // namespace gryce_engine::editor
