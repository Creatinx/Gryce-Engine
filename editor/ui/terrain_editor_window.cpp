#include "terrain_editor_window.h"

#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>

#include <imgui.h>

#include "components/mesh_renderer.h"
#include "scene/entity.h"
#include "resources/project.h"
#include "resources/resource_path.h"
#include "utils/glog/glog_lib.h"
#include "../localization/localization.h"

namespace gryce_engine::editor {

namespace {

// 导出 MeshData 到 OBJ 文件；返回是否成功
bool export_to_obj(const assets::MeshData& mesh, const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::ofstream file(path);
    if (!file.is_open()) return false;

    file << "# Gryce Engine generated terrain mesh\n";
    for (const auto& v : mesh.vertices) {
        file << std::format("v {} {} {}\n", v.position.x, v.position.y, v.position.z);
    }
    for (const auto& v : mesh.vertices) {
        file << std::format("vn {} {} {}\n", v.normal.x, v.normal.y, v.normal.z);
    }
    for (const auto& v : mesh.vertices) {
        file << std::format("vt {} {}\n", v.uv.x, v.uv.y);
    }

    for (size_t i = 0; i < mesh.indices.size(); i += 3) {
        uint32_t i0 = mesh.indices[i] + 1;
        uint32_t i1 = mesh.indices[i + 1] + 1;
        uint32_t i2 = mesh.indices[i + 2] + 1;
        file << std::format("f {}/{}/{} {}/{}/{} {}/{}/{}\n", i0, i0, i0, i1, i1, i1, i2, i2, i2);
    }
    return true;
}

} // namespace

void TerrainEditorWindow::open(scene::Entity* entity, components::Terrain* terrain) {
    entity_ = entity;
    terrain_ = terrain;
    open_ = (entity != nullptr && terrain != nullptr);
    sync_preview_resolution();
}

void TerrainEditorWindow::draw() {
    if (!open_ || !terrain_ || !entity_) return;

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(520.0f, 620.0f), ImGuiCond_Appearing);

    bool still_open = true;
    if (ImGui::Begin(tr("terrain_editor.title"), &still_open, ImGuiWindowFlags_NoDocking)) {
        ImGui::Text("%s: %s", tr("common.name"), entity_->name().c_str());
        ImGui::Separator();

        draw_parameters();
        ImGui::Separator();
        draw_heightmap_preview();
        ImGui::Separator();
        draw_actions();
    }
    ImGui::End();

    if (!still_open) {
        open_ = false;
        entity_ = nullptr;
        terrain_ = nullptr;
    }
}

void TerrainEditorWindow::draw_parameters() {
    ImGui::Text("%s", tr("terrain_editor.parameters"));

    bool changed = false;
    changed |= ImGui::DragFloat(tr("terrain_editor.width"), &terrain_->width, 1.0f, 1.0f, 10000.0f);
    changed |= ImGui::DragFloat(tr("terrain_editor.depth"), &terrain_->depth, 1.0f, 1.0f, 10000.0f);

    int res = terrain_->resolution;
    if (ImGui::DragInt(tr("terrain_editor.resolution"), &res, 1.0f, 2, 512)) {
        terrain_->resolution = res;
        terrain_->generate_noise();
        sync_preview_resolution();
        changed = true;
    }

    changed |= ImGui::DragFloat(tr("terrain_editor.height_scale"), &terrain_->height_scale, 0.1f, 0.0f, 1000.0f);

    int seed = terrain_->seed;
    if (ImGui::DragInt(tr("terrain_editor.seed"), &seed, 1, 0, 1000000)) {
        terrain_->seed = seed;
        terrain_->generate_noise();
        changed = true;
    }

    char buf[256] = {};
    std::strncpy(buf, terrain_->base_texture_path.c_str(), sizeof(buf) - 1);
    if (ImGui::InputText(tr("terrain_editor.base_texture"), buf, sizeof(buf))) {
        terrain_->base_texture_path = buf;
        changed = true;
    }

    if (changed) {
        // 标记组件已编辑（无 Undo 集成，因为高度图是容器，暂不在反射系统支持范围内）
    }
}

void TerrainEditorWindow::draw_heightmap_preview() {
    ImGui::Text("%s", tr("terrain_editor.heightmap"));

    const int s = terrain_->heightmap_size();
    if (s <= 1) return;

    const ImVec2 canvas_p0 = ImGui::GetCursorScreenPos();
    const float max_size = 280.0f;
    const float cell = max_size / static_cast<float>(s);
    const ImVec2 canvas_sz(cell * static_cast<float>(s), cell * static_cast<float>(s));

    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(canvas_p0, ImVec2(canvas_p0.x + canvas_sz.x, canvas_p0.y + canvas_sz.y),
                        IM_COL32(40, 40, 40, 255));

    for (int z = 0; z < s; ++z) {
        for (int x = 0; x < s; ++x) {
            float h = terrain_->height_at(x, z);
            uint8_t c = static_cast<uint8_t>(h * 255.0f);
            ImVec2 min_p(canvas_p0.x + x * cell, canvas_p0.y + z * cell);
            ImVec2 max_p(min_p.x + cell, min_p.y + cell);
            draw->AddRectFilled(min_p, max_p, IM_COL32(c, c, c, 255));
        }
    }

    ImGui::Dummy(canvas_sz);

    // 高度图简单绘制：点击抬起高度
    if (ImGui::IsItemHovered() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        ImVec2 mouse = ImGui::GetMousePos();
        int ix = static_cast<int>((mouse.x - canvas_p0.x) / cell);
        int iz = static_cast<int>((mouse.y - canvas_p0.y) / cell);
        if (ix >= 0 && ix < s && iz >= 0 && iz < s) {
            float h = terrain_->height_at(ix, iz);
            h = std::min(h + 0.05f, 1.0f);
            terrain_->set_height(ix, iz, h);
        }
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
        ImVec2 mouse = ImGui::GetMousePos();
        int ix = static_cast<int>((mouse.x - canvas_p0.x) / cell);
        int iz = static_cast<int>((mouse.y - canvas_p0.y) / cell);
        if (ix >= 0 && ix < s && iz >= 0 && iz < s) {
            float h = terrain_->height_at(ix, iz);
            h = std::max(h - 0.05f, 0.0f);
            terrain_->set_height(ix, iz, h);
        }
    }

    ImGui::TextDisabled("%s", tr("terrain_editor.paint_hint"));
}

void TerrainEditorWindow::draw_actions() {
    if (ImGui::Button(tr("terrain_editor.generate_noise"))) {
        terrain_->generate_noise();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("terrain_editor.normalize"))) {
        terrain_->normalize_heights();
    }

    if (ImGui::Button(tr("terrain_editor.create_mesh_renderer"))) {
        if (export_mesh_and_create_renderer()) {
            GLOG_INFO("Terrain editor: created MeshRenderer for entity '{}'", entity_->name());
        }
    }
}

void TerrainEditorWindow::sync_preview_resolution() {
    preview_resolution_ = terrain_ ? terrain_->resolution : 64;
}

bool TerrainEditorWindow::export_mesh_and_create_renderer() {
    if (!entity_ || !terrain_) return false;

    std::filesystem::path root = resources::Project::instance().root();
    if (root.empty()) {
        GLOG_ERROR("Terrain editor: project root is empty, cannot export mesh");
        return false;
    }

    std::filesystem::path models_dir = root / "models";
    std::error_code ec;
    std::filesystem::create_directories(models_dir, ec);

    // 寻找不重复的文件名
    std::filesystem::path out_path;
    for (int i = 0; i < 1000; ++i) {
        std::string name = std::format("terrain_{}_{:03d}.obj",
                                       entity_->name().empty() ? "entity" : entity_->name(), i);
        std::filesystem::path candidate = models_dir / name;
        if (!std::filesystem::exists(candidate, ec)) {
            out_path = candidate;
            break;
        }
    }
    if (out_path.empty()) {
        GLOG_ERROR("Terrain editor: failed to find available output file");
        return false;
    }

    assets::MeshData mesh = terrain_->build_mesh_data();
    if (!export_to_obj(mesh, out_path)) {
        GLOG_ERROR("Terrain editor: failed to write OBJ '{}'", out_path.string());
        return false;
    }

    std::string res_path = resources::ResourcePath::make_relative(out_path.string(), root.string());

    auto* mr = entity_->get_component<components::MeshRenderer>();
    if (!mr) {
        mr = entity_->add_component<components::MeshRenderer>(res_path);
    } else {
        mr->mesh_path = res_path;
    }

    if (mr && mr->material && !terrain_->base_texture_path.empty()) {
        mr->material->albedo_map_path = terrain_->base_texture_path;
        mr->material->use_albedo_map = true;
    }

    return true;
}

} // namespace gryce_engine::editor
