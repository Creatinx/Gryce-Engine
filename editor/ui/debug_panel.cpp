#include "debug_panel.h"

#include <cstring>
#include <format>
#include <imgui.h>

#include "../localization/localization.h"
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
    ImGui::Text("%s", tr("debug.material"));

    char name_buf[128] = {};
    std::strncpy(name_buf, material->name.c_str(), sizeof(name_buf) - 1);
    name_buf[sizeof(name_buf) - 1] = '\0';
    if (ImGui::InputText(tr("common.name"), name_buf, sizeof(name_buf))) {
        material->name = name_buf;
    }

    float color[3] = {material->albedo_color.x, material->albedo_color.y, material->albedo_color.z};
    if (ImGui::ColorEdit3(tr("debug.albedo"), color)) {
        material->albedo_color = math::Vector3f(color[0], color[1], color[2]);
    }

    ImGui::SliderFloat(tr("debug.roughness"), &material->roughness, 0.0f, 1.0f);
    ImGui::SliderFloat(tr("debug.metallic"), &material->metallic, 0.0f, 1.0f);
    ImGui::SliderFloat(tr("debug.ao"), &material->ao, 0.0f, 1.0f);

    auto texture_field = [](const char* label, std::string& path, bool& use_flag) {
        char checkbox_label[64] = {};
        std::snprintf(checkbox_label, sizeof(checkbox_label), tr("debug.use_map"), label);
        ImGui::Checkbox(checkbox_label, &use_flag);

        char buf[256] = {};
        std::strncpy(buf, path.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        if (ImGui::InputText(label, buf, sizeof(buf))) {
            path = buf;
        }
    };

    texture_field(tr("debug.albedo_map"), material->albedo_map_path, material->use_albedo_map);
    texture_field(tr("debug.normal_map"), material->normal_map_path, material->use_normal_map);
    texture_field(tr("debug.roughness_map"), material->roughness_map_path, material->use_roughness_map);
    texture_field(tr("debug.metallic_map"), material->metallic_map_path, material->use_metallic_map);
    texture_field(tr("debug.ao_map"), material->ao_map_path, material->use_ao_map);
}

void draw_camera_editor(components::Camera* cam) {
    if (!cam) return;
    ImGui::Separator();
    ImGui::Text("%s", tr("debug.camera"));
    ImGui::Checkbox(tr("debug.is_main"), &cam->is_main);
    ImGui::SliderFloat(tr("debug.fov"), &cam->fov, 10.0f, 120.0f);
    ImGui::DragFloat(tr("debug.near"), &cam->near_plane, 0.01f, 0.001f, 10.0f);
    ImGui::DragFloat(tr("debug.far"), &cam->far_plane, 1.0f, 10.0f, 10000.0f);
    float bg[4] = { cam->background_color.r, cam->background_color.g,
                    cam->background_color.b, cam->background_color.a };
    if (ImGui::ColorEdit4(tr("debug.background"), bg)) {
        cam->background_color = render::Color(bg[0], bg[1], bg[2], bg[3]);
    }
}

void draw_light_editor(components::Light* light) {
    if (!light) return;
    ImGui::Separator();
    ImGui::Text("%s", tr("debug.light"));
    const char* types[] = { tr("common.light_directional"), tr("common.light_point"), tr("common.light_spot") };
    int type_idx = static_cast<int>(light->light_type);
    if (ImGui::Combo(tr("common.type"), &type_idx, types, IM_ARRAYSIZE(types))) {
        light->light_type = static_cast<components::Light::Type>(type_idx);
    }
    float color[3] = { light->color.x, light->color.y, light->color.z };
    if (ImGui::ColorEdit3(tr("common.color"), color)) {
        light->color = math::Vector3f(color[0], color[1], color[2]);
    }
    ImGui::DragFloat(tr("debug.intensity"), &light->intensity, 0.1f, 0.0f, 100.0f);
    ImGui::DragFloat3(tr("debug.direction"), &light->direction.x, 0.01f);
    ImGui::DragFloat(tr("debug.range"), &light->range, 0.1f, 0.0f, 1000.0f);
    ImGui::SliderFloat(tr("debug.spot_angle"), &light->spot_angle, 1.0f, 179.0f);
}

void draw_node2d_editor(components::Node2D* node) {
    if (!node) return;
    ImGui::Separator();
    ImGui::Text("%s", tr("debug.node2d"));
    ImGui::DragInt(tr("debug.z_index"), &node->z_index);
    ImGui::Checkbox(tr("debug.top_level"), &node->top_level);
}

void draw_node3d_editor(components::Node3D* node) {
    if (!node) return;
    ImGui::Separator();
    ImGui::Text("%s", tr("debug.node3d"));
    ImGui::Checkbox(tr("debug.visible"), &node->visible);
}

void draw_rigid_body_editor(components::RigidBody* rb) {
    if (!rb) return;
    ImGui::Separator();
    ImGui::Text("%s", tr("debug.rigidbody"));
    ImGui::DragFloat(tr("debug.mass"), &rb->mass, 0.1f, 0.001f, 10000.0f);
    ImGui::Checkbox(tr("debug.use_gravity"), &rb->use_gravity);
    ImGui::Checkbox(tr("debug.kinematic"), &rb->is_kinematic);
    ImGui::DragFloat3(tr("debug.velocity"), &rb->velocity.x, 0.1f);
    ImGui::SliderFloat(tr("debug.restitution"), &rb->restitution, 0.0f, 1.0f);
    ImGui::SliderFloat(tr("debug.friction"), &rb->friction, 0.0f, 1.0f);
    ImGui::DragFloat(tr("debug.linear_damping"), &rb->linear_damping, 0.01f, 0.0f, 1.0f);
    ImGui::DragFloat(tr("debug.angular_damping"), &rb->angular_damping, 0.01f, 0.0f, 1.0f);
}

void draw_static_body_editor(components::StaticBody* sb) {
    if (!sb) return;
    ImGui::Separator();
    ImGui::Text("%s", tr("debug.staticbody"));
    ImGui::Checkbox(tr("debug.kinematic"), &sb->kinematic);
}

void draw_box_collider_editor(components::BoxCollider* col) {
    if (!col) return;
    ImGui::Separator();
    ImGui::Text("%s", tr("debug.boxcollider"));
    ImGui::DragFloat3(tr("debug.size"), &col->size.x, 0.01f);
    ImGui::DragFloat3(tr("debug.center"), &col->center.x, 0.01f);
    ImGui::Checkbox(tr("debug.trigger"), &col->is_trigger);
}

void draw_sphere_collider_editor(components::SphereCollider* col) {
    if (!col) return;
    ImGui::Separator();
    ImGui::Text("%s", tr("debug.spherecollider"));
    ImGui::DragFloat(tr("debug.radius"), &col->radius, 0.01f, 0.0f, 1000.0f);
    ImGui::DragFloat3(tr("debug.center"), &col->center.x, 0.01f);
    ImGui::Checkbox(tr("debug.trigger"), &col->is_trigger);
}

void draw_audio_source_editor(components::AudioSource* audio) {
    if (!audio) return;
    ImGui::Separator();
    ImGui::Text("%s", tr("debug.audiosource"));
    char buf[256] = {};
    std::strncpy(buf, audio->clip_path.c_str(), sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    if (ImGui::InputText(tr("debug.clip_path"), buf, sizeof(buf))) {
        audio->clip_path = buf;
    }
    ImGui::SliderFloat(tr("debug.volume"), &audio->volume, 0.0f, 1.0f);
    ImGui::DragFloat(tr("debug.pitch"), &audio->pitch, 0.01f, 0.1f, 3.0f);
    ImGui::Checkbox(tr("debug.loop"), &audio->loop);
    ImGui::Checkbox(tr("debug.play_on_awake"), &audio->play_on_awake);
    ImGui::Checkbox(tr("debug.is_3d"), &audio->is_3d);
    ImGui::DragFloat(tr("debug.min_distance"), &audio->min_distance, 0.1f, 0.0f, 1000.0f);
    ImGui::DragFloat(tr("debug.max_distance"), &audio->max_distance, 0.1f, 0.0f, 10000.0f);
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
    ImGui::Begin(tr("debug.title"));

    if (window) {
        double fps = window->fps();
        double delta_ms = window->delta_time() * 1000.0;
        ImGui::TextUnformatted(std::vformat(tr("debug.fps"), std::make_format_args(fps)).c_str());
        ImGui::TextUnformatted(std::vformat(tr("debug.delta_ms"), std::make_format_args(delta_ms)).c_str());
    }

    if (camera) {
        math::Vector3f pos = camera->position();
        double cx = static_cast<double>(pos.x);
        double cy = static_cast<double>(pos.y);
        double cz = static_cast<double>(pos.z);
        ImGui::TextUnformatted(std::vformat(tr("debug.camera_position"),
                                            std::make_format_args(cx, cy, cz)).c_str());
    }

    // -----------------------------------------------------------------------
    // Frame Pacing / GPU 控制
    // -----------------------------------------------------------------------
    ImGui::Separator();
    ImGui::Text("%s", tr("debug.frame_pacing"));

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

    if (ImGui::Checkbox(tr("debug.limit_fps"), &limiter_enabled)) {
        if (frame_limiter) {
            frame_limiter->set_enabled(limiter_enabled);
            if (limiter_enabled) {
                // 启用时立即应用当前 Target FPS，避免 limiter 实际目标还是 0
                frame_limiter->set_target_fps(target_fps);
            }
        }
    }
    if (ImGui::InputInt(tr("debug.target_fps"), &target_fps)) {
        if (target_fps < 0) target_fps = 0;
        if (target_fps > 1000) target_fps = 1000;
        if (frame_limiter && limiter_enabled) {
            frame_limiter->set_target_fps(target_fps);
        }
    }

    if (ImGui::Checkbox(tr("debug.vsync"), &vsync_enabled)) {
        if (render_ctx) {
            render_ctx->set_swap_interval(vsync_enabled ? 1 : 0);
        }
    }

    if (ImGui::Checkbox(tr("debug.gpu_busy_spin"), &gpu_busy_enabled)) {
        if (render_ctx) {
            render_ctx->set_gpu_busy_spin(gpu_busy_enabled, gpu_busy_iterations);
        }
    }
    if (ImGui::SliderInt(tr("debug.busy_iterations"), &gpu_busy_iterations, 0, 64)) {
        if (render_ctx && gpu_busy_enabled) {
            render_ctx->set_gpu_busy_spin(true, gpu_busy_iterations);
        }
    }

    if (nv_delay_supported) {
        if (ImGui::SliderFloat(tr("debug.nv_delay"), &nv_delay_ms, 0.0f, 20.0f, "%.2f")) {
            if (render_ctx) {
                render_ctx->set_nv_delay_before_swap(nv_delay_ms / 1000.0f);
            }
        }
    } else {
        ImGui::TextDisabled(tr("debug.nv_delay_not_supported"));
    }

    // -----------------------------------------------------------------------
    // 输入 / 渲染方向修正（避免我再瞎猜）
    // -----------------------------------------------------------------------
    ImGui::Separator();
    ImGui::Text("%s", tr("debug.input_cull_toggles"));
    ImGui::Checkbox(tr("debug.invert_mouse_y"), &invert_mouse_y_);
    ImGui::Checkbox(tr("debug.swap_space_ctrl"), &swap_space_ctrl_);
    ImGui::Checkbox(tr("debug.disable_cull"), &disable_cull_);
    if (pipeline) {
        bool grid_enabled = pipeline->grid_enabled();
        if (ImGui::Checkbox(tr("debug.grid"), &grid_enabled)) {
            pipeline->set_grid_enabled(grid_enabled);
        }
    }
    if (ImGui::Button(tr("debug.reset_input_defaults"))) {
        invert_mouse_y_ = false;
        swap_space_ctrl_ = false;
        disable_cull_   = false;
    }

    // -----------------------------------------------------------------------
    // Shadow Map
    // -----------------------------------------------------------------------
    ImGui::Separator();
    ImGui::Text("%s", tr("debug.shadow_map"));
    static float shadow_bias = 0.005f;
    static bool use_shadow = true;
    if (ImGui::SliderFloat(tr("debug.shadow_bias"), &shadow_bias, 0.0001f, 0.05f, "%.4f")) {
        if (pipeline) {
            pipeline->set_shadow_bias(shadow_bias);
        }
    }
    if (ImGui::Checkbox(tr("debug.use_shadow"), &use_shadow)) {
        // TODO: 开关 shadow map 需要管线支持；目前仅保存状态供后续使用
    }

    ImGui::Separator();
    ImGui::Text("%s", tr("debug.scene_hierarchy"));
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
    const char* entity_name = entity->name().c_str();
    ImGui::TextUnformatted(std::vformat(tr("debug.inspector"), std::make_format_args(entity_name)).c_str());

    auto* t = entity->transform();
    if (t) {
        ImGui::DragFloat3(tr("debug.position"), &t->position.x, 0.1f);
        ImGui::Text("%s", tr("debug.rotation_tbd"));
        ImGui::DragFloat3(tr("debug.scale"), &t->scale.x, 0.01f);
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
        ImGui::Text("%s", tr("debug.meshrenderer"));
        char buf[256] = {};
        std::strncpy(buf, mr->mesh_path.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        if (ImGui::InputText(tr("debug.mesh_path"), buf, sizeof(buf))) {
            mr->mesh_path = buf;
        }
        draw_material_editor(mr->ensure_material());
    }

    ImGui::Separator();
    ImGui::Text("%s", tr("debug.add_component"));
    add_component_button<components::Camera>(entity, std::format("+{}", tr("debug.camera")).c_str());
    add_component_button<components::Light>(entity, std::format("+{}", tr("debug.light")).c_str());
    add_component_button<components::Node2D>(entity, std::format("+{}", tr("debug.node2d")).c_str());
    add_component_button<components::Node3D>(entity, std::format("+{}", tr("debug.node3d")).c_str());
    add_component_button<components::StaticBody>(entity, std::format("+{}", tr("debug.staticbody")).c_str());
    add_component_button<components::RigidBody>(entity, std::format("+{}", tr("debug.rigidbody")).c_str());
    add_component_button<components::BoxCollider>(entity, std::format("+{}", tr("debug.boxcollider")).c_str());
    add_component_button<components::SphereCollider>(entity, std::format("+{}", tr("debug.spherecollider")).c_str());
    add_component_button<components::AudioSource>(entity, std::format("+{}", tr("debug.audiosource")).c_str());
    if (!entity->get_component<components::MeshRenderer>()) {
        if (ImGui::Button(std::format("+{}", tr("debug.meshrenderer")).c_str())) {
            entity->add_component<components::MeshRenderer>("res:/models/cube_pbr.obj");
        }
    }
}

bool ModelLoaderPanel::show(scene::Scene* scene) {
    ImGui::SetNextWindowPos(ImVec2(950.0f, 10.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(320.0f, 120.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin(tr("debug.model_loader"));

    ImGui::InputText(tr("debug.model_path"), path_buffer_, sizeof(path_buffer_));

    bool loaded = false;
    if (ImGui::Button(tr("debug.load_model"))) {
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
    ImGui::TextUnformatted(tr("debug.supported_formats"));

    ImGui::End();
    return loaded;
}

} // namespace gryce_engine::editor::ui
