#include "debug_panel.h"

#include <cstring>
#include <imgui.h>

#include "components/mesh_renderer.h"
#include "components/node2d.h"
#include "components/node3d.h"
#include "components/camera.h"
#include "components/light.h"
#include "components/static_body.h"
#include "components/rigid_body.h"
#include "components/box_collider.h"
#include "components/sphere_collider.h"
#include "components/audio_source.h"
#include "utils/glog/glog_lib.h"
#include "components/transform.h"
#include "math/camera.h"
#include "platform/window.h"
#include "render/material.h"
#include "render/render_context.h"
#include "render/render_pipeline.h"
#include "scene/entity.h"
#include "scene/scene.h"
#include "utils/frame_limiter.h"

namespace gryce_engine::editor::ui {

namespace {

void draw_material_editor(render::Material* material) {
    if (!material) return;

    ImGui::Separator();
    ImGui::Text("Material");

    char name_buf[128] = {};
    std::strncpy(name_buf, material->name.c_str(), sizeof(name_buf) - 1);
    name_buf[sizeof(name_buf) - 1] = '\0';
    if (ImGui::InputText("Name", name_buf, sizeof(name_buf))) {
        material->name = name_buf;
    }

    float color[3] = {material->albedo_color.x, material->albedo_color.y, material->albedo_color.z};
    if (ImGui::ColorEdit3("Albedo", color)) {
        material->albedo_color = math::Vector3f(color[0], color[1], color[2]);
    }

    ImGui::SliderFloat("Roughness", &material->roughness, 0.0f, 1.0f);
    ImGui::SliderFloat("Metallic", &material->metallic, 0.0f, 1.0f);
    ImGui::SliderFloat("AO", &material->ao, 0.0f, 1.0f);

    // 高级材质：Clearcoat / Sheen / 各向异性
    ImGui::Separator();
    ImGui::Text("Advanced BRDF");
    ImGui::SliderFloat("Clearcoat", &material->clearcoat, 0.0f, 1.0f);
    ImGui::SliderFloat("Clearcoat Roughness", &material->clearcoat_roughness, 0.02f, 1.0f);
    ImGui::SliderFloat("Sheen", &material->sheen, 0.0f, 1.0f);
    float sheen_tint[3] = {material->sheen_tint.x, material->sheen_tint.y, material->sheen_tint.z};
    if (ImGui::ColorEdit3("Sheen Tint", sheen_tint)) {
        material->sheen_tint = math::Vector3f(sheen_tint[0], sheen_tint[1], sheen_tint[2]);
    }
    ImGui::SliderFloat("Anisotropy", &material->anisotropy, -1.0f, 1.0f);
    ImGui::SliderFloat("Anisotropy Rotation", &material->anisotropy_rotation, -3.14159f, 3.14159f);

    auto texture_field = [](const char* label, std::string& path, bool& use_flag) {
        char checkbox_label[64] = {};
        std::snprintf(checkbox_label, sizeof(checkbox_label), "Use %s", label);
        ImGui::Checkbox(checkbox_label, &use_flag);

        char buf[256] = {};
        std::strncpy(buf, path.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        if (ImGui::InputText(label, buf, sizeof(buf))) {
            path = buf;
        }
    };

    texture_field("Albedo Map", material->albedo_map_path, material->use_albedo_map);
    texture_field("Normal Map", material->normal_map_path, material->use_normal_map);
    texture_field("Roughness Map", material->roughness_map_path, material->use_roughness_map);
    texture_field("Metallic Map", material->metallic_map_path, material->use_metallic_map);
    texture_field("AO Map", material->ao_map_path, material->use_ao_map);
}

void draw_camera_editor(components::Camera* cam) {
    if (!cam) return;
    ImGui::Separator();
    ImGui::Text("Camera");
    ImGui::Checkbox("Is Main", &cam->is_main);
    ImGui::SliderFloat("FOV", &cam->fov, 10.0f, 120.0f);
    ImGui::DragFloat("Near", &cam->near_plane, 0.01f, 0.001f, 10.0f);
    ImGui::DragFloat("Far", &cam->far_plane, 1.0f, 10.0f, 10000.0f);
    float bg[4] = { cam->background_color.r, cam->background_color.g,
                    cam->background_color.b, cam->background_color.a };
    if (ImGui::ColorEdit4("Background", bg)) {
        cam->background_color = render::Color(bg[0], bg[1], bg[2], bg[3]);
    }
}

void draw_light_editor(components::Light* light) {
    if (!light) return;
    ImGui::Separator();
    ImGui::Text("Light");
    const char* types[] = { "Directional", "Point", "Spot" };
    int type_idx = static_cast<int>(light->light_type);
    if (ImGui::Combo("Type", &type_idx, types, IM_ARRAYSIZE(types))) {
        light->light_type = static_cast<components::Light::Type>(type_idx);
    }
    float color[3] = { light->color.x, light->color.y, light->color.z };
    if (ImGui::ColorEdit3("Color", color)) {
        light->color = math::Vector3f(color[0], color[1], color[2]);
    }
    ImGui::DragFloat("Intensity", &light->intensity, 0.1f, 0.0f, 100.0f);
    ImGui::DragFloat3("Direction", &light->direction.x, 0.01f);
    ImGui::DragFloat("Range", &light->range, 0.1f, 0.0f, 1000.0f);
    ImGui::SliderFloat("Spot Angle", &light->spot_angle, 1.0f, 179.0f);
}

void draw_node2d_editor(components::Node2D* node) {
    if (!node) return;
    ImGui::Separator();
    ImGui::Text("Node2D");
    ImGui::DragInt("Z-Index", &node->z_index);
    ImGui::Checkbox("Top Level", &node->top_level);
}

void draw_node3d_editor(components::Node3D* node) {
    if (!node) return;
    ImGui::Separator();
    ImGui::Text("Node3D");
    ImGui::Checkbox("Visible", &node->visible);
}

void draw_rigid_body_editor(components::RigidBody* rb) {
    if (!rb) return;
    ImGui::Separator();
    ImGui::Text("RigidBody");
    ImGui::DragFloat("Mass", &rb->mass, 0.1f, 0.001f, 10000.0f);
    ImGui::Checkbox("Use Gravity", &rb->use_gravity);
    ImGui::Checkbox("Kinematic", &rb->is_kinematic);
    ImGui::DragFloat3("Velocity", &rb->velocity.x, 0.1f);
    ImGui::SliderFloat("Restitution", &rb->restitution, 0.0f, 1.0f);
    ImGui::SliderFloat("Friction", &rb->friction, 0.0f, 1.0f);
    ImGui::DragFloat("Linear Damping", &rb->linear_damping, 0.01f, 0.0f, 1.0f);
    ImGui::DragFloat("Angular Damping", &rb->angular_damping, 0.01f, 0.0f, 1.0f);
}

void draw_static_body_editor(components::StaticBody* sb) {
    if (!sb) return;
    ImGui::Separator();
    ImGui::Text("StaticBody");
    ImGui::Checkbox("Kinematic", &sb->kinematic);
}

void draw_box_collider_editor(components::BoxCollider* col) {
    if (!col) return;
    ImGui::Separator();
    ImGui::Text("BoxCollider");
    ImGui::DragFloat3("Size", &col->size.x, 0.01f);
    ImGui::DragFloat3("Center", &col->center.x, 0.01f);
    ImGui::Checkbox("Trigger", &col->is_trigger);
}

void draw_sphere_collider_editor(components::SphereCollider* col) {
    if (!col) return;
    ImGui::Separator();
    ImGui::Text("SphereCollider");
    ImGui::DragFloat("Radius", &col->radius, 0.01f, 0.0f, 1000.0f);
    ImGui::DragFloat3("Center", &col->center.x, 0.01f);
    ImGui::Checkbox("Trigger", &col->is_trigger);
}

void draw_audio_source_editor(components::AudioSource* audio) {
    if (!audio) return;
    ImGui::Separator();
    ImGui::Text("AudioSource");
    char buf[256] = {};
    std::strncpy(buf, audio->clip_path.c_str(), sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    if (ImGui::InputText("Clip Path", buf, sizeof(buf))) {
        audio->clip_path = buf;
    }
    ImGui::SliderFloat("Volume", &audio->volume, 0.0f, 1.0f);
    ImGui::DragFloat("Pitch", &audio->pitch, 0.01f, 0.1f, 3.0f);
    ImGui::Checkbox("Loop", &audio->loop);
    ImGui::Checkbox("Play On Awake", &audio->play_on_awake);
    ImGui::Checkbox("3D", &audio->is_3d);
    ImGui::DragFloat("Min Distance", &audio->min_distance, 0.1f, 0.0f, 1000.0f);
    ImGui::DragFloat("Max Distance", &audio->max_distance, 0.1f, 0.0f, 10000.0f);
}

template<typename T>
void add_component_button(scene::Entity* entity, const char* label) {
    if (entity->get_component<T>()) return;
    if (ImGui::Button(label)) {
        entity->add_component<T>();
    }
    ImGui::SameLine();
}

} // namespace

void DebugPanel::show(platform::Window* window, scene::Scene* scene, math::Camera* camera,
                      utils::FrameLimiter* frame_limiter, render::RenderContext* render_ctx,
                      render::RenderPipeline* pipeline) {
    // 强制标准 FPS 默认值，避免旧状态/ini 干扰
    static bool first_frame = true;
    if (first_frame) {
        invert_mouse_y_ = false;
        swap_space_ctrl_ = false;
        disable_cull_   = false;
        first_frame = false;
        GLOG_INFO("DebugPanel input defaults applied: invert_mouse_y=false, swap_space_ctrl=false, disable_cull=false");
    }

    ImGui::SetNextWindowPos(ImVec2(200.0f, 10.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(320.0f, 480.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("Debug");

    if (window) {
        ImGui::Text("FPS: %.1f", window->fps());
        ImGui::Text("Delta: %.3f ms", window->delta_time() * 1000.0);
    }

    if (camera) {
        math::Vector3f pos = camera->position();
        ImGui::Text("Camera: (%.2f, %.2f, %.2f)", static_cast<double>(pos.x),
                    static_cast<double>(pos.y), static_cast<double>(pos.z));
    }

    // -----------------------------------------------------------------------
    // Frame Pacing / GPU 控制
    // -----------------------------------------------------------------------
    ImGui::Separator();
    ImGui::Text("Frame Pacing");

    // 与 FrameLimiter 的真实状态保持同步（首次显示时初始化）
    static bool limiter_enabled = frame_limiter ? (frame_limiter->enabled() && frame_limiter->target_fps() > 0) : false;
    static int target_fps = frame_limiter ? (frame_limiter->target_fps() > 0 ? frame_limiter->target_fps() : 120) : 120;
    static bool vsync_enabled = false;
    static bool gpu_busy_enabled = false;
    static int gpu_busy_iterations = 4;
    static bool nv_delay_supported = false;
    static float nv_delay_ms = 0.0f;

    if (render_ctx) {
        nv_delay_supported = render_ctx->supports_nv_delay_before_swap();
    }

    if (ImGui::Checkbox("Limit FPS", &limiter_enabled)) {
        if (frame_limiter) {
            frame_limiter->set_enabled(limiter_enabled);
            if (limiter_enabled) {
                // 启用时立即应用当前 Target FPS，避免 limiter 实际目标还是 0
                frame_limiter->set_target_fps(target_fps);
            }
        }
    }
    if (ImGui::InputInt("Target FPS", &target_fps)) {
        if (target_fps < 0) target_fps = 0;
        if (target_fps > 1000) target_fps = 1000;
        if (frame_limiter && limiter_enabled) {
            frame_limiter->set_target_fps(target_fps);
        }
    }

    if (ImGui::Checkbox("VSync", &vsync_enabled)) {
        if (render_ctx) {
            render_ctx->set_swap_interval(vsync_enabled ? 1 : 0);
        }
    }

    if (ImGui::Checkbox("GPU Busy Spin", &gpu_busy_enabled)) {
        if (render_ctx) {
            render_ctx->set_gpu_busy_spin(gpu_busy_enabled, gpu_busy_iterations);
        }
    }
    if (ImGui::SliderInt("Busy Iterations", &gpu_busy_iterations, 0, 64)) {
        if (render_ctx && gpu_busy_enabled) {
            render_ctx->set_gpu_busy_spin(true, gpu_busy_iterations);
        }
    }

    if (nv_delay_supported) {
        if (ImGui::SliderFloat("NV Delay Before Swap (ms)", &nv_delay_ms, 0.0f, 20.0f, "%.2f")) {
            if (render_ctx) {
                render_ctx->set_nv_delay_before_swap(nv_delay_ms / 1000.0f);
            }
        }
    } else {
        ImGui::TextDisabled("WGL_NV_delay_before_swap: not supported");
    }

    // -----------------------------------------------------------------------
    // 输入 / 渲染方向修正（避免我再瞎猜）
    // -----------------------------------------------------------------------
    ImGui::Separator();
    ImGui::Text("Input / Cull Toggles");
    ImGui::Checkbox("Invert Mouse Y", &invert_mouse_y_);
    ImGui::Checkbox("Swap Space/Ctrl", &swap_space_ctrl_);
    ImGui::Checkbox("Disable Cull", &disable_cull_);
    if (ImGui::Button("Reset Input Defaults")) {
        invert_mouse_y_ = false;
        swap_space_ctrl_ = false;
        disable_cull_   = false;
    }

    // -----------------------------------------------------------------------
    // Shadow Map
    // -----------------------------------------------------------------------
    ImGui::Separator();
    ImGui::Text("Shadow Map");
    static float shadow_bias = 0.001f;
    static bool use_shadow = true;
    if (ImGui::SliderFloat("Shadow Bias", &shadow_bias, 0.0001f, 0.01f, "%.4f")) {
        if (pipeline) {
            pipeline->set_shadow_bias(shadow_bias);
        }
    }
    if (ImGui::Checkbox("Use Shadow", &use_shadow)) {
        if (pipeline) {
            pipeline->set_shadow_enabled(use_shadow);
        }
    }
    // CSM 级联阴影
    static int cascade_count = pipeline ? pipeline->cascade_count() : 3;
    static float cascade_lambda = pipeline ? pipeline->cascade_split_lambda() : 0.5f;
    if (ImGui::SliderInt("Cascades", &cascade_count, 1, 4)) {
        if (pipeline) pipeline->set_cascade_count(cascade_count);
    }
    if (ImGui::SliderFloat("Split Lambda", &cascade_lambda, 0.0f, 1.0f, "%.2f")) {
        if (pipeline) pipeline->set_cascade_split_lambda(cascade_lambda);
    }
    static float normal_offset = 1.0f;
    if (ImGui::SliderFloat("Normal Offset", &normal_offset, 0.0f, 4.0f, "%.1f")) {
        if (pipeline) pipeline->set_normal_offset_scale(normal_offset);
    }
    // PCSS 软阴影
    static bool pcss_enabled = pipeline && pipeline->pcss_enabled();
    static float pcss_light_size = 8.0f;
    static float pcss_max_radius = 16.0f;
    if (ImGui::Checkbox("PCSS Soft Shadows", &pcss_enabled)) {
        if (pipeline) pipeline->set_pcss_enabled(pcss_enabled);
    }
    bool pcss_changed = ImGui::SliderFloat("PCSS Light Size", &pcss_light_size, 1.0f, 40.0f, "%.1f");
    pcss_changed |= ImGui::SliderFloat("PCSS Max Radius", &pcss_max_radius, 1.0f, 40.0f, "%.1f");
    if (pcss_changed && pipeline) {
        pipeline->set_pcss_params(pcss_light_size, pcss_max_radius);
    }
    // 屏幕空间接触阴影（补 Peter-Panning 脚底黑）
    static bool contact_shadow_enabled = pipeline && pipeline->contact_shadow_enabled();
    static float cs_strength = 0.6f;
    static float cs_radius = 0.5f;
    static int cs_steps = 4;
    if (ImGui::Checkbox("Contact Shadow", &contact_shadow_enabled)) {
        if (pipeline) pipeline->set_contact_shadow_enabled(contact_shadow_enabled);
    }
    if (ImGui::SliderFloat("CS Strength", &cs_strength, 0.0f, 1.0f, "%.2f")) {
        if (pipeline) pipeline->set_contact_shadow_params(cs_strength, cs_radius, cs_steps);
    }
    if (ImGui::SliderFloat("CS Radius", &cs_radius, 0.05f, 2.0f, "%.2f")) {
        if (pipeline) pipeline->set_contact_shadow_params(cs_strength, cs_radius, cs_steps);
    }
    if (ImGui::SliderInt("CS Steps", &cs_steps, 1, 16)) {
        if (pipeline) pipeline->set_contact_shadow_params(cs_strength, cs_radius, cs_steps);
    }

    // -----------------------------------------------------------------------
    // HDR / Tone Mapping
    // -----------------------------------------------------------------------
    ImGui::Separator();
    ImGui::Text("HDR / Tone Mapping");
    if (pipeline) {
        if (ImGui::Button("Rebuild Pipeline (Hot Reload)")) {
            // 只置标记，真正重建由主循环在 present() 之后执行（需暂停渲染线程）
            pipeline_reload_requested_ = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(F5)");
        ImGui::Separator();
    }
    static float exposure = 1.0f;
    static int tone_map_mode = 1; // 0: none, 1: Reinhard, 2: ACES, 3: AgX, 4: Filmic
    if (ImGui::SliderFloat("Exposure", &exposure, 0.1f, 5.0f, "%.2f")) {
        if (pipeline) {
            pipeline->set_exposure(exposure);
        }
    }
    const char* k_tone_map_names[] = { "None", "Reinhard", "ACES", "AgX", "Filmic" };
    if (ImGui::Combo("Tone Map", &tone_map_mode, k_tone_map_names, 5)) {
        if (pipeline) {
            pipeline->set_tone_map_mode(tone_map_mode);
        }
    }
    static float contrast = 1.0f;
    static float saturation = 1.0f;
    bool grade_changed = ImGui::SliderFloat("Contrast", &contrast, 0.2f, 2.0f, "%.2f");
    grade_changed |= ImGui::SliderFloat("Saturation", &saturation, 0.0f, 2.0f, "%.2f");
    if (grade_changed && pipeline) {
        render::PostProcessParams pp = pipeline->tonemap_params();
        pp.contrast = contrast;
        pp.saturation = saturation;
        pipeline->set_tonemap_params(pp);
    }

    // Bloom 后处理
    static bool bloom_enabled = pipeline ? pipeline->bloom_enabled() : true;
    static float bloom_threshold = 1.0f;
    static float bloom_intensity = 0.35f;
    if (ImGui::Checkbox("Bloom", &bloom_enabled)) {
        if (pipeline) pipeline->set_bloom_enabled(bloom_enabled);
    }
    bool bloom_changed = ImGui::SliderFloat("Bloom Threshold", &bloom_threshold, 0.1f, 3.0f, "%.2f");
    bloom_changed |= ImGui::SliderFloat("Bloom Intensity", &bloom_intensity, 0.0f, 2.0f, "%.2f");
    if (bloom_changed && pipeline) {
        pipeline->set_bloom_params(bloom_threshold, bloom_intensity);
    }

    // 轻量镜头效果
    static float film_grain = 0.0f;
    static float vignette = 0.0f;
    static float chromatic_aberration = 0.0f;
    bool fx_changed = ImGui::SliderFloat("Film Grain", &film_grain, 0.0f, 0.2f, "%.3f");
    fx_changed |= ImGui::SliderFloat("Vignette", &vignette, 0.0f, 1.0f, "%.2f");
    fx_changed |= ImGui::SliderFloat("Chromatic Aberration", &chromatic_aberration, 0.0f, 1.0f, "%.2f");
    if (fx_changed && pipeline) {
        render::PostProcessParams pp = pipeline->tonemap_params();
        pp.film_grain = film_grain;
        pp.vignette = vignette;
        pp.chromatic_aberration = chromatic_aberration;
        pipeline->set_tonemap_params(pp);
    }

    // HDR 分析视图
    ImGui::Separator();
    ImGui::Text("HDR Analysis View");
    static int debug_view = 0;
    const char* k_debug_view_names[] = {
        "Final", "Albedo", "Normal", "Roughness", "Metallic",
        "Shadow", "Direct", "Indirect", "Cascade",
    };
    if (ImGui::Combo("View Mode", &debug_view, k_debug_view_names, 9)) {
        if (pipeline) pipeline->set_debug_view(debug_view);
    }

    // 时间性抗锯齿 / 自动曝光 / LUT / 物理光照单位
    ImGui::Separator();
    ImGui::Text("Temporal / Exposure / Color");
    static bool taa_enabled = pipeline && pipeline->taa_enabled();
    static float taa_weight = 0.85f;
    if (ImGui::Checkbox("TAA", &taa_enabled)) {
        if (pipeline) pipeline->set_taa_enabled(taa_enabled);
    }
    if (ImGui::SliderFloat("TAA Weight", &taa_weight, 0.5f, 0.95f, "%.2f")) {
        if (pipeline) pipeline->set_taa_weight(taa_weight);
    }

    static bool ssao_enabled = pipeline && pipeline->ssao_enabled();
    static float ssao_strength = 1.0f;
    static float ssao_radius = 12.0f;
    if (ImGui::Checkbox("GTAO/SSAO", &ssao_enabled)) {
        if (pipeline) pipeline->set_ssao_enabled(ssao_enabled);
    }
    bool ssao_changed = ImGui::SliderFloat("SSAO Strength", &ssao_strength, 0.0f, 2.0f, "%.2f");
    ssao_changed |= ImGui::SliderFloat("SSAO Radius", &ssao_radius, 2.0f, 40.0f, "%.1f");
    if (ssao_changed && pipeline) {
        pipeline->set_ssao_params(ssao_strength, ssao_radius);
    }

    static bool auto_exposure = pipeline && pipeline->auto_exposure();
    static float ae_target = 0.18f;
    static float ae_min = 0.1f;
    static float ae_max = 4.0f;
    static float ae_speed = 1.0f;
    if (ImGui::Checkbox("Auto Exposure", &auto_exposure)) {
        if (pipeline) pipeline->set_auto_exposure(auto_exposure);
    }
    bool ae_changed = ImGui::SliderFloat("AE Target", &ae_target, 0.02f, 1.0f, "%.3f");
    ae_changed |= ImGui::SliderFloat("AE Min", &ae_min, 0.02f, 2.0f, "%.2f");
    ae_changed |= ImGui::SliderFloat("AE Max", &ae_max, 0.5f, 8.0f, "%.2f");
    ae_changed |= ImGui::SliderFloat("AE Speed", &ae_speed, 0.05f, 1.0f, "%.2f");
    if (ae_changed && pipeline) {
        pipeline->set_auto_exposure_params(ae_target, ae_min, ae_max, ae_speed);
    }

    static bool lut_enabled = false;
    static float lut_strength = 1.0f;
    static char lut_path[256] = "";
    if (ImGui::Checkbox("3D LUT", &lut_enabled)) {
        if (pipeline) pipeline->set_lut_enabled(lut_enabled);
    }
    if (ImGui::SliderFloat("LUT Strength", &lut_strength, 0.0f, 1.0f, "%.2f")) {
        if (pipeline) pipeline->set_lut_strength(lut_strength);
    }
    ImGui::InputText("LUT Path", lut_path, sizeof(lut_path));
    if (ImGui::Button("Load LUT")) {
        if (pipeline && lut_path[0]) pipeline->set_color_lut(lut_path);
    }

    static bool physical_units = pipeline && pipeline->light_units_physical();
    if (ImGui::Checkbox("Physical Light Units", &physical_units)) {
        if (pipeline) pipeline->set_light_units_physical(physical_units);
    }

    ImGui::Separator();
    ImGui::Text("Scene Hierarchy");
    if (scene) {
        for (const auto& root : scene->roots()) {
            draw_scene_hierarchy(root.get());
        }
    }

    if (selected_entity_) {
        ImGui::Separator();
        draw_entity_inspector(selected_entity_);
    }

    ImGui::End();
}

void DebugPanel::draw_scene_hierarchy(scene::Entity* entity) {
    if (!entity) return;

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow;
    if (entity->children().empty()) {
        flags |= ImGuiTreeNodeFlags_Leaf;
    }
    if (entity == selected_entity_) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }

    bool open = ImGui::TreeNodeEx(entity->name().c_str(), flags);
    if (ImGui::IsItemClicked()) {
        selected_entity_ = entity;
    }

    if (open) {
        for (const auto& child : entity->children()) {
            draw_scene_hierarchy(child.get());
        }
        ImGui::TreePop();
    }
}

void DebugPanel::draw_entity_inspector(scene::Entity* entity) {
    ImGui::Text("Inspector: %s", entity->name().c_str());

    auto* t = entity->transform();
    if (t) {
        ImGui::DragFloat3("Position", &t->position.x, 0.1f);
        ImGui::Text("Rotation: (quaternion editing TBD)");
        ImGui::DragFloat3("Scale", &t->scale.x, 0.01f);
    }

    draw_camera_editor(entity->get_component<components::Camera>());
    draw_light_editor(entity->get_component<components::Light>());
    draw_node2d_editor(entity->get_component<components::Node2D>());
    draw_node3d_editor(entity->get_component<components::Node3D>());
    draw_static_body_editor(entity->get_component<components::StaticBody>());
    draw_rigid_body_editor(entity->get_component<components::RigidBody>());
    draw_box_collider_editor(entity->get_component<components::BoxCollider>());
    draw_sphere_collider_editor(entity->get_component<components::SphereCollider>());
    draw_audio_source_editor(entity->get_component<components::AudioSource>());

    auto* mr = entity->get_component<components::MeshRenderer>();
    if (mr) {
        ImGui::Separator();
        ImGui::Text("MeshRenderer");
        char buf[256] = {};
        std::strncpy(buf, mr->mesh_path.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        if (ImGui::InputText("Mesh Path", buf, sizeof(buf))) {
            mr->mesh_path = buf;
        }
        draw_material_editor(mr->ensure_material());
    }

    ImGui::Separator();
    ImGui::Text("Add Component");
    add_component_button<components::Camera>(entity, "+Camera");
    add_component_button<components::Light>(entity, "+Light");
    add_component_button<components::Node2D>(entity, "+Node2D");
    add_component_button<components::Node3D>(entity, "+Node3D");
    add_component_button<components::StaticBody>(entity, "+StaticBody");
    add_component_button<components::RigidBody>(entity, "+RigidBody");
    add_component_button<components::BoxCollider>(entity, "+BoxCollider");
    add_component_button<components::SphereCollider>(entity, "+SphereCollider");
    add_component_button<components::AudioSource>(entity, "+AudioSource");
    if (!entity->get_component<components::MeshRenderer>()) {
        if (ImGui::Button("+MeshRenderer")) {
            entity->add_component<components::MeshRenderer>("res:/models/cube_pbr.obj");
        }
    }
}

bool ModelLoaderPanel::show(scene::Scene* scene) {
    ImGui::SetNextWindowPos(ImVec2(950.0f, 10.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(320.0f, 120.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("Model Loader");

    ImGui::InputText("Path", path_buffer_, sizeof(path_buffer_));

    bool loaded = false;
    if (ImGui::Button("Load Model")) {
        if (scene) {
            scene::Entity* e = scene->create_entity("LoadedModel");
            e->transform()->position = math::Vector3f(0.0f, 0.0f, 0.0f);
            auto* mr = e->add_component<components::MeshRenderer>(std::string(path_buffer_));
            if (mr && mr->material) {
                mr->material->name = "LoadedModelMat";
            }
            loaded = true;
        }
    }

    ImGui::SameLine();
    ImGui::Text("(OBJ/FBX/glTF/DAE/PLY/STL)");

    ImGui::End();
    return loaded;
}

} // namespace gryce_engine::editor::ui
