#include "material_editor_window.h"

#include <cstring>
#include <format>

#include <imgui.h>

#include "../localization/localization.h"
#include "file_browser_popup.h"
#include "components/mesh_renderer.h"
#include "components/skinned_mesh_renderer.h"
#include "render/render_context.h"
#include "scene/entity.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::editor {

namespace {

void copy_to_buf(char* buf, size_t size, const std::string& str) {
    std::strncpy(buf, str.c_str(), size - 1);
    buf[size - 1] = '\0';
}

void input_texture_slot(const char* label, char* buf, size_t buf_size,
                        std::string& path, bool& use_flag) {
    // 三列表格：勾选框 / 输入框（拉伸）/「浏览...」按钮（固定宽），按钮不会被挤出
    if (!ImGui::BeginTable(std::format("{}##row", label).c_str(), 3)) return;
    ImGui::TableSetupColumn("##use", ImGuiTableColumnFlags_WidthFixed,
                            ImGui::GetFrameHeight());
    ImGui::TableSetupColumn("##input", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("##browsebtn", ImGuiTableColumnFlags_WidthFixed,
                            FileBrowserPopup::browse_button_width());

    ImGui::TableNextColumn();
    ImGui::Checkbox(std::format("{}##use", label).c_str(), &use_flag);

    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN); // 填满单元格（自动为 label 留位）
    if (ImGui::InputText(std::format("{}##path", label).c_str(), buf, buf_size)) {
        path = buf;
        use_flag = !path.empty();
    }
    // 支持从 Project 面板拖拽资源文件到贴图槽
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload =
                ImGui::AcceptDragDropPayload("GRYCE_PROJECT_FILE", ImGuiDragDropFlags_AcceptBeforeDelivery)) {
            if (payload->IsDelivery() && payload->DataSize > 0) {
                path = std::string(static_cast<const char*>(payload->Data));
                copy_to_buf(buf, buf_size, path);
                use_flag = !path.empty();
            }
        }
        ImGui::EndDragDropTarget();
    }

    // 「浏览...」按钮：弹窗选择 res:/ 资源
    ImGui::TableNextColumn();
    ImGui::PushID(label);
    if (FileBrowserPopup::instance().browse_button("##browse", buf, buf_size)) {
        path = buf;
        use_flag = !path.empty();
    }
    ImGui::PopID();

    ImGui::EndTable();
}

} // namespace

void MaterialEditorWindow::open(render::Material* material, scene::Entity* owner,
                                const std::string& asset_path, SaveCallback on_save,
                                render::RenderContext* ctx) {
    material_ = material;
    owner_ = owner;
    asset_path_ = asset_path;
    on_save_ = std::move(on_save);
    ctx_ = ctx;
    open_ = true;
    sync_buffers_from_material();
}

std::string MaterialEditorWindow::title() const {
    if (!material_) return tr("material_editor.title");
    return std::format("{}: {}", tr("material_editor.title"), material_->name);
}

void MaterialEditorWindow::sync_buffers_from_material() {
    if (!material_) return;
    copy_to_buf(name_buf_, sizeof(name_buf_), material_->name);
    copy_to_buf(albedo_buf_, sizeof(albedo_buf_), material_->albedo_map_path);
    copy_to_buf(normal_buf_, sizeof(normal_buf_), material_->normal_map_path);
    copy_to_buf(roughness_buf_, sizeof(roughness_buf_), material_->roughness_map_path);
    copy_to_buf(metallic_buf_, sizeof(metallic_buf_), material_->metallic_map_path);
    copy_to_buf(ao_buf_, sizeof(ao_buf_), material_->ao_map_path);
    copy_to_buf(emissive_buf_, sizeof(emissive_buf_), material_->emissive_map_path);
}

void MaterialEditorWindow::save_material() {
    if (!material_) return;

    if (on_save_) {
        on_save_(material_);
    }

    if (!asset_path_.empty()) {
        if (material_->save_to_file(asset_path_)) {
            GLOG_INFO("MaterialEditor: saved material to '{}'", asset_path_);
        }
    } else if (owner_ && ctx_) {
        // 实体材质：销毁旧 GPU 纹理并立即重新上传，使修改即时生效。
        bool belongs_to_owner = false;
        if (auto* mr = owner_->get_component<components::MeshRenderer>()) {
            belongs_to_owner = (mr->material.get() == material_);
        }
        if (!belongs_to_owner) {
            if (auto* smr = owner_->get_component<components::SkinnedMeshRenderer>()) {
                belongs_to_owner = (smr->material.get() == material_);
            }
        }
        if (belongs_to_owner) {
            material_->destroy_gpu(ctx_);
            material_->upload_to_gpu(ctx_);
            GLOG_INFO("MaterialEditor: re-uploaded GPU textures for '{}'", material_->name);
        }
    }
}

void MaterialEditorWindow::draw() {
    if (!open_ || !material_) return;

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(460.0f, 640.0f), ImGuiCond_Appearing);

    bool still_open = true;
    if (ImGui::Begin(title().c_str(), &still_open,
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_MenuBar)) {
        // 菜单栏
        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu(tr("common.file"))) {
                if (ImGui::MenuItem(tr("common.save"))) {
                    save_material();
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }

        // 基础信息
        if (ImGui::InputText(tr("common.name"), name_buf_, sizeof(name_buf_))) {
            material_->name = name_buf_;
        }

        const char* blend_labels[] = { "Opaque", "Blend" };
        int blend_idx = static_cast<int>(material_->blend_mode);
        if (ImGui::Combo(tr("material_editor.blend_mode"), &blend_idx, blend_labels, IM_ARRAYSIZE(blend_labels))) {
            material_->blend_mode = static_cast<render::Material::BlendMode>(blend_idx);
        }
        ImGui::Checkbox(tr("material_editor.two_sided"), &material_->two_sided);

        ImGui::Separator();
        ImGui::Text("%s", tr("material_editor.surface_inputs"));

        float color[3] = { material_->albedo_color.x, material_->albedo_color.y, material_->albedo_color.z };
        if (ImGui::ColorEdit3(tr("material_editor.albedo"), color)) {
            material_->albedo_color = math::Vector3f(color[0], color[1], color[2]);
        }
        ImGui::SliderFloat(tr("material_editor.roughness"), &material_->roughness, 0.0f, 1.0f);
        ImGui::SliderFloat(tr("material_editor.metallic"), &material_->metallic, 0.0f, 1.0f);
        ImGui::SliderFloat(tr("material_editor.ao"), &material_->ao, 0.0f, 1.0f);

        float emissive[3] = { material_->emissive_color.x, material_->emissive_color.y, material_->emissive_color.z };
        if (ImGui::ColorEdit3(tr("material_editor.emissive"), emissive)) {
            material_->emissive_color = math::Vector3f(emissive[0], emissive[1], emissive[2]);
        }
        ImGui::SliderFloat(tr("material_editor.opacity"), &material_->opacity, 0.0f, 1.0f);

        float uv_scale[2] = { material_->uv_scale.x, material_->uv_scale.y };
        if (ImGui::DragFloat2(tr("material_editor.uv_scale"), uv_scale, 0.01f)) {
            material_->uv_scale = math::Vector2f(uv_scale[0], uv_scale[1]);
        }
        float uv_offset[2] = { material_->uv_offset.x, material_->uv_offset.y };
        if (ImGui::DragFloat2(tr("material_editor.uv_offset"), uv_offset, 0.01f)) {
            material_->uv_offset = math::Vector2f(uv_offset[0], uv_offset[1]);
        }

        ImGui::Separator();
        ImGui::Text("%s", tr("material_editor.maps"));

        input_texture_slot(tr("material_editor.albedo_map"), albedo_buf_, sizeof(albedo_buf_),
                           material_->albedo_map_path, material_->use_albedo_map);
        input_texture_slot(tr("material_editor.normal_map"), normal_buf_, sizeof(normal_buf_),
                           material_->normal_map_path, material_->use_normal_map);
        input_texture_slot(tr("material_editor.roughness_map"), roughness_buf_, sizeof(roughness_buf_),
                           material_->roughness_map_path, material_->use_roughness_map);
        input_texture_slot(tr("material_editor.metallic_map"), metallic_buf_, sizeof(metallic_buf_),
                           material_->metallic_map_path, material_->use_metallic_map);
        input_texture_slot(tr("material_editor.ao_map"), ao_buf_, sizeof(ao_buf_),
                           material_->ao_map_path, material_->use_ao_map);
        input_texture_slot(tr("material_editor.emissive_map"), emissive_buf_, sizeof(emissive_buf_),
                           material_->emissive_map_path, material_->use_emissive_map);

        ImGui::Separator();
        ImGui::Text("%s", tr("material_editor.physical_properties"));
        ImGui::SliderFloat(tr("material_editor.softness"), &material_->softness, 0.0f, 1.0f);
        ImGui::DragFloat(tr("material_editor.drag_coefficient"), &material_->drag_coefficient, 0.001f, 0.0f, 1.0f);
        ImGui::DragFloat(tr("material_editor.density"), &material_->density, 0.01f, 0.0f, 1000.0f);

        ImGui::Separator();
        if (ImGui::Button(tr("common.save"), ImVec2(120.0f, 0.0f))) {
            save_material();
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("common.close"), ImVec2(120.0f, 0.0f))) {
            still_open = false;
        }
    }
    ImGui::End();

    if (!still_open) {
        open_ = false;
        material_ = nullptr;
        owner_ = nullptr;
        on_save_ = nullptr;
    }
}

} // namespace gryce_engine::editor
