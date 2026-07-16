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
#include "components/physical_material.h"
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
    static float shadow_bias = 0.005f;
    static bool use_shadow = true;
    if (ImGui::SliderFloat("Shadow Bias", &shadow_bias, 0.0001f, 0.05f, "%.4f")) {
        if (pipeline) {
            pipeline->set_shadow_bias(shadow_bias);
        }
    }
    if (ImGui::Checkbox("Use Shadow", &use_shadow)) {
        // TODO: 开关 shadow map 需要管线支持；目前仅保存状态供后续使用
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
            // 从空中生成并附带完整物理组件：刚体 + 盒碰撞体 + 物理材质，
            // 加载后即可下落、碰撞、被重力枪拾取。
            e->transform()->position = math::Vector3f(0.0f, 20.0f, 0.0f);
            auto* mr = e->add_component<components::MeshRenderer>(std::string(path_buffer_));
            if (mr && mr->material) {
                mr->material->name = "LoadedModelMat";
            }
            e->add_component<components::RigidBody>();
            e->add_component<components::BoxCollider>();
            auto* pm = e->add_component<components::PhysicalMaterial>();
            if (pm) {
                pm->apply_preset("Wood");
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
