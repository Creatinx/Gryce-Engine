#include "editor_app.h"

#include <iostream>
#include <format>
#include <fstream>
#include <vector>
#include <string>
#include <memory>
#include <filesystem>
#include "stb/stb_image_write.h"
#include <thread>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <utility>
#include <future>
#include <algorithm>

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#endif

#include <GL/glew.h>
#include <GLFW/glfw3.h>

#ifdef _WIN32
#include <GLFW/glfw3native.h>
#include <windows.h>
#endif

#include <imgui.h>
#include <ImGuizmo.h>

#include "platform/window.h"
#include "platform/input.h"
#include "render/render_context.h"
#include "render/rhi_handle.h"
#include "render/mesh.h"
#include "render/shader.h"
#include "render/render2d.h"
#include "render/opengl/imgui_renderer.h"
#include "components/2d/basic_rect.h"
#include "components/2d/shape.h"
#include "components/2d/label.h"
#include "components/mesh_renderer.h"
#include "components/skinned_mesh_renderer.h"
#include "components/physics_body.h"
#include "components/node2d.h"
#include "components/node3d.h"
#include "components/camera.h"
#include "components/light.h"
#include "components/static_body.h"
#include "components/rigid_body.h"
#include "components/box_collider.h"
#include "components/sphere_collider.h"
#include "components/physical_material.h"
#include "components/audio_source.h"
#include "components/component_factory.h"
#include "assets/asset_manager.h"
#include "assets/mesh_data.h"
#include "resources/project.h"
#include "resources/resource_path.h"
#include "scene/scene.h"
#include "scene/scene_serializer.h"
#include "scene/entity.h"
#include "scene/prefab.h"
#include "math/math.h"
#include "math/ray.h"
#include "math/camera.h"
#include "utils/glog/glog_lib.h"
#include "utils/frame_limiter.h"
#include "ecs/world.h"
#include "ecs/systems/physics_system_3d.h"
#include "ecs/systems/render_system_2d.h"
#include "ecs/systems/render_system_3d.h"
#include "render/render_pipeline.h"

#include "cli_args.h"
#include "editor_camera.h"
#include "panel_manager.h"
#include "project/project_settings.h"
#include "recorder.h"
#include "panels/hierarchy_panel.h"
#include "panels/inspector_panel.h"
#include "panels/console_panel.h"
#include "panels/viewport_panel.h"
#include "panels/game_view_panel.h"
#include "panels/project_panel.h"
#include "ui/editor_theme.h"
#include "ui/settings_window.h"
#include "ui/project_settings_window.h"
#include "ui/gimport_editor_window.h"
#include "ui/message_popup.h"
#include "import/gimport_settings.h"
#include "assets_manager/asset_database.h"
#include "localization/localization.h"
#include "shortcuts/shortcut_manager.h"
#include "undo/command_stack.h"
#include "undo/commands.h"
#include "fluent_window.h"

using namespace gryce_engine;

// ---------------------------------------------------------------------------
// 查找编辑器项目根目录：
// 1) 先定位引擎仓库根（含 CMakeLists.txt 与 core/）；
// 2) 优先使用 editor/project/（编辑器自带项目，含 project.gryce）；
// 3) 缺失时回退 examples/3dtest/（开发期共享资源）；
// 4) 都找不到则退化为当前工作目录。
// ---------------------------------------------------------------------------
static std::filesystem::path find_project_root() {
    std::filesystem::path exe_path;
#ifdef _WIN32
    wchar_t buffer[MAX_PATH];
    if (GetModuleFileNameW(nullptr, buffer, MAX_PATH) > 0) {
        exe_path = std::filesystem::path(buffer);
    }
#else
    exe_path = std::filesystem::canonical("/proc/self/exe");
#endif
    std::filesystem::path dir = exe_path.parent_path();
    std::filesystem::path engine_root;
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        if (std::filesystem::exists(dir / "CMakeLists.txt") &&
            std::filesystem::is_directory(dir / "core")) {
            engine_root = dir;
            break;
        }
        dir = dir.parent_path();
    }
    if (!engine_root.empty()) {
        const std::filesystem::path editor_project = engine_root / "editor" / "project";
        if (std::filesystem::exists(editor_project / "project.gryce")) {
            return editor_project;
        }
        const std::filesystem::path dev_project = engine_root / "examples" / "3dtest";
        if (std::filesystem::exists(dev_project / "project.gryce")) {
            return dev_project;
        }
    }
    return std::filesystem::current_path();
}

// ---------------------------------------------------------------------------
// 场景辅助函数（与原 demo 相同：2D FPS label + 图形、物理演示 cube/ground）
// ---------------------------------------------------------------------------
static std::string resolve_scene_path(const std::string& input, const std::filesystem::path& project_root) {
    if (input.empty()) return {};
    if (input.rfind("res:/", 0) == 0) return input;

    std::filesystem::path p(input);
    if (p.is_absolute() && std::filesystem::exists(p)) return p.string();

    auto rel_to_project = project_root / input;
    if (std::filesystem::exists(rel_to_project)) return rel_to_project.string();

    std::error_code ec;
    auto abs = std::filesystem::absolute(p, ec);
    return ec ? input : abs.string();
}

static std::unique_ptr<scene::Scene> create_demo_scene(float screen_w, float screen_h) {
    auto scene = std::make_unique<scene::Scene>("main");

    // 左上角 FPS 背景
    {
        scene::Entity* fps_bg = scene->create_entity("FPS_BG");
        fps_bg->transform()->position = math::Vector3f(5.0f, 5.0f, 0.0f);
        fps_bg->add_component<components::d2::basic_rect::ColorRect>(120.0f, 30.0f, render::Color(0.2f, 0.2f, 0.2f, 0.8f));
    }

    // 左上角 FPS 文字
    {
        scene::Entity* fps_label = scene->create_entity("FPS_Label");
        fps_label->transform()->position = math::Vector3f(15.0f, 23.0f, 0.0f);
        fps_label->add_component<components::d2::text::Label>("FPS: 0.0", 18.0f, render::Color::white());
    }

    // 右下角图形测试面板
    float panel_w = 200.0f, panel_h = 200.0f;
    float panel_x = screen_w - panel_w - 20.0f;
    float panel_y = screen_h - panel_h - 20.0f;
    float panel_cx = panel_x + panel_w * 0.5f;
    float panel_cy = panel_y + panel_h * 0.5f;

    {
        scene::Entity* panel_bg = scene->create_entity("Panel_BG");
        panel_bg->transform()->position = math::Vector3f(panel_x, panel_y, 0.0f);
        panel_bg->add_component<components::d2::basic_rect::ColorRect>(panel_w, panel_h, render::Color::gray(0.15f));
    }

    {
        scene::Entity* circle = scene->create_entity("Circle");
        circle->transform()->position = math::Vector3f(panel_cx, panel_cy, 0.0f);
        circle->add_component<components::d2::shape::Circle>(50.0f, 32, render::Color::orange());
    }

    {
        scene::Entity* triangle = scene->create_entity("Triangle");
        triangle->transform()->position = math::Vector3f(panel_cx, panel_cy, 0.0f);
        std::vector<math::Vector2f> triangle_pts = {
            {-70.0f,  70.0f},
            {  0.0f, -70.0f},
            { 70.0f,  70.0f}
        };
        triangle->add_component<components::d2::shape::Polygon>(triangle_pts, render::Color::cyan());
    }

    {
        scene::Entity* pentagon = scene->create_entity("Pentagon");
        pentagon->transform()->position = math::Vector3f(panel_cx, panel_cy, 0.0f);
        std::vector<math::Vector2f> pentagon_pts;
        for (int i = 0; i < 5; ++i) {
            float angle = 2.0f * math::to_radians(72.0f) * static_cast<float>(i) - math::to_radians(90.0f);
            pentagon_pts.push_back({30.0f * std::cos(angle), 30.0f * std::sin(angle)});
        }
        pentagon->add_component<components::d2::shape::Polygon>(pentagon_pts, render::Color::magenta());
    }

    return scene;
}

static void upload_scene_meshes(scene::Scene& scene, render::RenderContext& ctx) {
    scene.foreach([&](scene::Entity* entity) {
        auto* mr = entity->get_component<components::MeshRenderer>();
        if (!mr || mr->mesh_path.empty()) return;

        auto data = assets::AssetManager::instance().load_mesh(mr->mesh_path);
        if (!data) return;

        render::IMesh* gpu_mesh = mr->upload_to_gpu(&ctx, data.get());
        if (gpu_mesh) {
            GLOG_INFO("Pre-uploaded mesh '{}' for entity '{}'", mr->mesh_path, entity->name());
        }
    });
}

static std::filesystem::file_time_type get_scene_write_time(const std::string& scene_path) {
    std::string resolved = resources::ResourcePath::resolve(scene_path);
    std::error_code ec;
    auto time = std::filesystem::last_write_time(resolved, ec);
    if (ec) {
        return std::filesystem::file_time_type::min();
    }
    return time;
}

static std::unique_ptr<scene::Scene> try_reload_scene(
    const std::string& scene_path,
    std::unique_ptr<scene::Scene> current,
    std::filesystem::file_time_type new_time,
    std::filesystem::file_time_type& last_write_time) {

    GLOG_INFO("Scene file changed, reloading '{}'", scene_path);
    auto reloaded = scene::SceneSerializer::load_from_file(scene_path);
    if (!reloaded) {
        GLOG_ERROR("Failed to reload scene from '{}'", scene_path);
        last_write_time = new_time; // 避免解析失败时反复重试
        return current;
    }

    last_write_time = new_time;
    GLOG_INFO("Scene reloaded successfully");
    return reloaded;
}

static scene::Entity* create_cube_entity(scene::Scene& scene, const std::string& name,
                                          bool dynamic = false) {
    scene::Entity* e = scene.create_entity(name);
    e->transform()->position = dynamic ? math::Vector3f(0.0f, 3.0f, 0.0f)
                                       : math::Vector3f(0.0f, 0.0f, 0.0f);
    auto* mr = e->add_component<components::MeshRenderer>("res:/models/cube_pbr.obj");
    if (mr && mr->material) {
        mr->material->name = dynamic ? "DynamicCube" : "CubePBR";
        mr->material->albedo_map_path = "res:/textures/cube_albedo.png";
        mr->material->normal_map_path = "res:/textures/cube_normal.png";
        mr->material->roughness_map_path = "res:/textures/cube_roughness.png";
        mr->material->metallic_map_path = "res:/textures/cube_metallic.png";
        mr->material->ao_map_path = "res:/textures/cube_ao.png";
    }

    if (dynamic) {
        e->add_component<components::RigidBody>();
    }
    return e;
}

static scene::Entity* create_ground_entity(scene::Scene& scene, const std::string& name) {
    scene::Entity* e = scene.create_entity(name);
    e->transform()->position = math::Vector3f(0.0f, -2.0f, 0.0f);
    e->transform()->scale = math::Vector3f(10.0f, 0.5f, 10.0f);

    auto* mr = e->add_component<components::MeshRenderer>("res:/models/cube_pbr.obj");
    if (mr && mr->material) {
        mr->material->name = "Ground";
        mr->material->use_albedo_map = false;
        mr->material->use_normal_map = false;
        mr->material->use_roughness_map = false;
        mr->material->use_metallic_map = false;
        mr->material->use_ao_map = false;
        mr->material->albedo_color = math::Vector3f(0.2f, 0.5f, 0.2f);
        mr->material->roughness = 0.9f;
        mr->material->metallic = 0.0f;
        mr->material->ao = 1.0f;
    }

    e->add_component<components::StaticBody>();
    return e;
}

static void ensure_physics_demo_entities(scene::Scene& scene) {
    if (!scene.find_entity_by_name("Ground")) {
        create_ground_entity(scene, "Ground");
    }

    scene::Entity* cube = scene.find_entity_by_name("Cube");
    if (!cube) {
        cube = create_cube_entity(scene, "Cube", true);
    } else {
        // 确保已有的 Cube 是动态的，并放到空中准备下落
        if (!cube->get_component<components::RigidBody>()) {
            cube->add_component<components::RigidBody>();
        }
        cube->transform()->position = math::Vector3f(0.0f, 3.0f, 0.0f);
        cube->transform()->rotation = math::Quaternionf::identity();
        cube->transform()->scale = math::Vector3f::one();
        if (auto* rb = cube->get_component<components::RigidBody>()) {
            rb->velocity = math::Vector3f::zero();
            rb->acceleration = math::Vector3f::zero();
        }
    }
}

// ---------------------------------------------------------------------------
// 场景默认对象：主摄像机 + 主光源
// ---------------------------------------------------------------------------
static scene::Entity* find_main_camera_entity(scene::Scene& scene) {
    scene::Entity* result = nullptr;
    scene::Entity* fallback_by_name = nullptr;
    scene::Entity* any_enabled = nullptr;

    scene.foreach([&](scene::Entity* entity) {
        auto* cam = entity->get_component<components::Camera>();
        if (!cam || !cam->enabled) return;

        if (!any_enabled) {
            any_enabled = entity;
        }
        if (!fallback_by_name && entity->name() == "MainCamera") {
            fallback_by_name = entity;
        }
        if (cam->is_main && !result) {
            result = entity;
        }
    });

    // 优先 is_main=true；未标记时按名称兜底；最后任选一启用相机，
    // 避免旧场景保存时 is_main 标志丢失导致编辑器相机无法同步。
    if (result) return result;
    if (fallback_by_name) return fallback_by_name;
    return any_enabled;
}

// 将 math::Camera 的朝向转换为 Transform 用的四元数。
// 注意：Quaternionf::from_euler 的约定与 Camera 的 (pitch=X, yaw=Y) 不一致，
// 因此直接根据相机的前/上/右向量构造旋转矩阵再转四元数，避免欧拉角歧义。
static math::Quaternionf camera_rotation_to_quaternion(const math::Camera& camera) {
    const math::Vector3f f = camera.forward();
    const math::Vector3f u = camera.up();
    const math::Vector3f r = camera.right();

    // 引擎的 from_rotation_matrix 按转置读取，因此这里传入的矩阵需为
    // 标准 local->world 旋转矩阵的转置。
    math::Matrix4f basis = math::Matrix4f::identity();
    basis(0, 0) = r.x;  basis(0, 1) = r.y;  basis(0, 2) = r.z;
    basis(1, 0) = u.x;  basis(1, 1) = u.y;  basis(1, 2) = u.z;
    basis(2, 0) = -f.x; basis(2, 1) = -f.y; basis(2, 2) = -f.z;
    return math::Quaternionf::from_rotation_matrix(basis);
}

// 将 Transform 四元数还原为 math::Camera 的 pitch/yaw。
// 注意：引擎的 Transform::local_matrix() 使用 q.to_matrix()，其矩阵乘法等价于
// q^-1 * v * q（与 rotate_vector 方向相反）。因此从 Transform 读取世界前向时应取
// conjugate().rotate_vector，否则会得到反向朝向，导致 Viewport 与 Game View 相反。
static void apply_quaternion_to_camera(const math::Quaternionf& rotation, math::Camera& camera) {
    const math::Vector3f forward = rotation.conjugate().rotate_vector(math::Vector3f::forward());
    const float pitch = math::to_degrees(std::asin(math::clamp(forward.y, -1.0f, 1.0f)));
    const float yaw = math::to_degrees(std::atan2(forward.z, forward.x));
    camera.set_pitch(pitch);
    camera.set_yaw(yaw);
}

// 将场景主摄像机的 Transform 同步到编辑器相机。
// 加载已有场景时，MainCamera 可能带有设计师指定的初始视角，编辑器相机应与之对齐，
// 否则 Viewport 与 Game View 会出现方向/位置不一致。
static void sync_editor_to_scene_camera(scene::Scene& scene, math::Camera& camera) {
    scene::Entity* cam_entity = find_main_camera_entity(scene);
    if (!cam_entity) return;

    camera.set_position(cam_entity->transform()->position);
    apply_quaternion_to_camera(cam_entity->transform()->rotation, camera);
}

static void ensure_scene_defaults(scene::Scene& scene, math::Camera& camera) {
    scene::Entity* cam_entity = find_main_camera_entity(scene);
    if (!cam_entity) {
        cam_entity = scene.create_entity("MainCamera");
        cam_entity->transform()->position = camera.position();
        cam_entity->transform()->rotation = camera_rotation_to_quaternion(camera);
        auto* cam = cam_entity->add_component<components::Camera>();
        cam->fov = camera.fov();
        cam->near_plane = 0.1f;
        cam->far_plane = 100.0f;
        cam->is_main = true;
        GLOG_INFO("Created default MainCamera entity");
    }

    bool has_light = false;
    scene.foreach([&](scene::Entity* entity) {
        if (entity->get_component<components::Light>()) has_light = true;
    });
    if (!has_light) {
        scene::Entity* light_entity = scene.create_entity("MainLight");
        light_entity->transform()->rotation = math::Quaternionf::from_euler(
            math::to_radians(-30.0f), math::to_radians(-45.0f), 0.0f);
        auto* light = light_entity->add_component<components::Light>();
        light->light_type = components::Light::Type::Directional;
        light->direction = math::Vector3f(-0.3f, -0.7f, -0.6f).normalized();
        light->color = math::Vector3f::one();
        light->intensity = 3.0f;
        GLOG_INFO("Created default MainLight entity");
    }
}

static void sync_active_camera_to_scene(scene::Scene& scene, math::Camera& camera) {
    scene::Entity* cam_entity = find_main_camera_entity(scene);
    if (!cam_entity) return;
    auto* cam = cam_entity->get_component<components::Camera>();
    if (!cam) return;

    // 将编辑器相机的位置/朝向写回 MainCamera 组件，便于保存场景。
    cam_entity->transform()->position = camera.position();
    cam_entity->transform()->rotation = camera_rotation_to_quaternion(camera);
}

static void apply_camera_component_to_global(scene::Scene& scene, math::Camera& camera) {
    scene::Entity* cam_entity = find_main_camera_entity(scene);
    if (!cam_entity) return;
    auto* cam = cam_entity->get_component<components::Camera>();
    if (!cam) return;
    camera.set_fov(cam->fov);
    camera.set_near_far(cam->near_plane, cam->far_plane);
}

// 从场景主摄像机构建 Game View 用的 math::Camera
static void build_game_camera(scene::Scene& scene, math::Camera& camera) {
    scene::Entity* cam_entity = find_main_camera_entity(scene);
    if (!cam_entity) return;
    auto* cam = cam_entity->get_component<components::Camera>();
    if (!cam) return;

    camera.set_fov(cam->fov);
    camera.set_near_far(cam->near_plane, cam->far_plane);
    camera.set_position(cam_entity->transform()->position);

    apply_quaternion_to_camera(cam_entity->transform()->rotation, camera);
}

static std::vector<render::RenderPipeline::Light> collect_lights(scene::Scene& scene) {
    std::vector<render::RenderPipeline::Light> lights;
    scene.foreach([&](scene::Entity* entity) {
        auto* light = entity->get_component<components::Light>();
        if (!light || !light->enabled) return;

        render::RenderPipeline::Light out;
        out.type = static_cast<render::RenderPipeline::LightType>(light->light_type);
        out.color = light->color;
        out.intensity = light->intensity;
        out.range = light->range;
        out.spot_angle = light->spot_angle;
        out.spot_softness = light->spot_softness;

        // 位置来自 Transform；方向光可忽略位置，点光/聚光必需。
        components::Transform* transform = entity->transform();
        out.position = transform ? transform->position : math::Vector3f::zero();

        // 方向由 Transform 旋转 + 组件局部方向共同决定。
        // 注意：Transform::local_matrix() 的 q.to_matrix() 等价于 q^-1 * v * q，
        // 因此世界空间方向需用 rotation.conjugate().rotate_vector，与相机朝向保持一致。
        math::Vector3f local_dir = light->direction.normalized();
        if (local_dir.length_sq() < 1e-6f) {
            local_dir = math::Vector3f(0.0f, -1.0f, 0.0f);
        }
        out.direction = (transform ? transform->rotation.conjugate().rotate_vector(local_dir) : local_dir).normalized();

        lights.push_back(out);
    });
    if (lights.empty()) {
        // 兜底：保证至少有一个方向光
        render::RenderPipeline::Light fallback;
        fallback.direction = math::Vector3f(0.0f, -1.0f, 0.0f);
        fallback.color = math::Vector3f::one();
        fallback.intensity = 1.0f;
        lights.push_back(fallback);
    }
    return lights;
}

// ---------------------------------------------------------------------------
// 点选拾取（M1-E2）：遍历场景中带 MeshRenderer 的实体，
// 用 mesh 顶点的本地 AABB 经世界矩阵变换成世界 AABB 后做射线求交，取最近命中。
// 不依赖碰撞体（无 Collider 的实体也能被选中）；编辑器规模下线扫开销可忽略。
// ---------------------------------------------------------------------------
static bool compute_world_aabb(const assets::MeshData& mesh, const math::Matrix4f& world,
                               math::Vector3f& out_min, math::Vector3f& out_max) {
    if (mesh.vertices.empty()) return false;

    math::Vector3f lo = mesh.vertices[0].position;
    math::Vector3f hi = lo;
    for (const auto& v : mesh.vertices) {
        lo.x = std::min(lo.x, v.position.x);
        lo.y = std::min(lo.y, v.position.y);
        lo.z = std::min(lo.z, v.position.z);
        hi.x = std::max(hi.x, v.position.x);
        hi.y = std::max(hi.y, v.position.y);
        hi.z = std::max(hi.z, v.position.z);
    }

    // 本地 AABB 8 角点经世界矩阵变换后重取包围盒
    out_min = math::Vector3f(1e30f, 1e30f, 1e30f);
    out_max = math::Vector3f(-1e30f, -1e30f, -1e30f);
    for (int i = 0; i < 8; ++i) {
        const math::Vector3f corner((i & 1) ? hi.x : lo.x,
                                    (i & 2) ? hi.y : lo.y,
                                    (i & 4) ? hi.z : lo.z);
        const math::Vector3f p = world.transform_point(corner);
        out_min.x = std::min(out_min.x, p.x);
        out_min.y = std::min(out_min.y, p.y);
        out_min.z = std::min(out_min.z, p.z);
        out_max.x = std::max(out_max.x, p.x);
        out_max.y = std::max(out_max.y, p.y);
        out_max.z = std::max(out_max.z, p.z);
    }
    return true;
}

static bool compute_entity_world_bounds(scene::Entity* entity, math::Vector3f& out_center,
                                        float& out_radius);

// 计算整个场景的世界空间包围盒中心与包围球半径。
static bool compute_scene_bounds(scene::Scene& scene, math::Vector3f& out_center,
                                 float& out_radius) {
    math::Vector3f bmin(1e30f, 1e30f, 1e30f);
    math::Vector3f bmax(-1e30f, -1e30f, -1e30f);
    bool has_bounds = false;

    scene.foreach([&](scene::Entity* entity) {
        math::Vector3f center;
        float radius = 0.0f;
        if (!compute_entity_world_bounds(entity, center, radius)) return;
        bmin = math::Vector3f(std::min(bmin.x, center.x - radius),
                              std::min(bmin.y, center.y - radius),
                              std::min(bmin.z, center.z - radius));
        bmax = math::Vector3f(std::max(bmax.x, center.x + radius),
                              std::max(bmax.y, center.y + radius),
                              std::max(bmax.z, center.z + radius));
        has_bounds = true;
    });

    if (!has_bounds) return false;
    out_center = (bmin + bmax) * 0.5f;
    out_radius = (bmax - out_center).length();
    return true;
}

// 计算单个实体的世界空间包围盒中心与包围球半径。
// 优先使用 MeshRenderer / SkinnedMeshRenderer 的网格数据；没有网格时返回 false，
// 调用方可退回到 Transform.position。
static bool compute_entity_world_bounds(scene::Entity* entity, math::Vector3f& out_center,
                                        float& out_radius) {
    if (!entity) return false;

    math::Vector3f bmin(1e30f, 1e30f, 1e30f);
    math::Vector3f bmax(-1e30f, -1e30f, -1e30f);
    bool has_bounds = false;

    if (auto* mr = entity->get_component<components::MeshRenderer>()) {
        if (mr->enabled && !mr->mesh_path.empty()) {
            auto mesh = assets::AssetManager::instance().load_mesh(mr->mesh_path);
            if (mesh && compute_world_aabb(*mesh, entity->world_transform(), bmin, bmax)) {
                has_bounds = true;
            }
        }
    }

    if (auto* smr = entity->get_component<components::SkinnedMeshRenderer>()) {
        if (smr->enabled && smr->model() && !smr->model()->meshes.empty()) {
            math::Vector3f smin(1e30f, 1e30f, 1e30f);
            math::Vector3f smax(-1e30f, -1e30f, -1e30f);
            bool has_sub_bounds = false;
            for (const auto& mesh : smr->model()->meshes) {
                math::Vector3f sub_min, sub_max;
                if (!compute_world_aabb(mesh, entity->world_transform(), sub_min, sub_max)) continue;
                smin = math::Vector3f(std::min(smin.x, sub_min.x), std::min(smin.y, sub_min.y),
                                      std::min(smin.z, sub_min.z));
                smax = math::Vector3f(std::max(smax.x, sub_max.x), std::max(smax.y, sub_max.y),
                                      std::max(smax.z, sub_max.z));
                has_sub_bounds = true;
            }
            if (has_sub_bounds) {
                if (has_bounds) {
                    bmin = math::Vector3f(std::min(bmin.x, smin.x), std::min(bmin.y, smin.y),
                                          std::min(bmin.z, smin.z));
                    bmax = math::Vector3f(std::max(bmax.x, smax.x), std::max(bmax.y, smax.y),
                                          std::max(bmax.z, smax.z));
                } else {
                    bmin = smin;
                    bmax = smax;
                    has_bounds = true;
                }
            }
        }
    }

    if (!has_bounds) return false;

    out_center = (bmin + bmax) * 0.5f;
    const math::Vector3f extent = bmax - out_center;
    out_radius = extent.length();
    return true;
}

static scene::Entity* pick_entity(scene::Scene& scene, const math::Ray& ray) {
    scene::Entity* best_entity = nullptr;
    float best_t = 1e30f;

    scene.foreach([&](scene::Entity* entity) {
        auto* mr = entity->get_component<components::MeshRenderer>();
        if (!mr || !mr->enabled || mr->mesh_path.empty()) return;

        auto mesh = assets::AssetManager::instance().load_mesh(mr->mesh_path);
        if (!mesh) return;

        math::Vector3f bmin, bmax;
        if (!compute_world_aabb(*mesh.get(), entity->world_transform(), bmin, bmax)) return;

        float t = 0.0f;
        if (math::ray_intersect_aabb(ray, bmin, bmax, t) && t < best_t) {
            best_t = t;
            best_entity = entity;
        }
    });
    return best_entity;
}

namespace gryce_engine::editor {

// ---------------------------------------------------------------------------
// EditorApp::run — 编辑器主循环
// ---------------------------------------------------------------------------
int EditorApp::run(int argc, char* argv[]) {
    std::cout << "Gryce Engine Editor v0.1.0" << std::endl;

    // 解析命令行参数
    CliArgs args = parse_cli_args(argc, argv);
    if (args.show_help) return 0;

    bool api_override_by_cli = false;
    render::RenderAPI selected_api = render::RenderAPI::Vulkan;
    bool vulkan_validation = false; // 默认关闭 validation，需要时通过 --vulkan-validation 开启
    bool test_play_mode = false;    // --test-play-mode：自动进入/退出 Play Mode，用于 CI
    bool test_delete_undo = false;  // --test-delete-undo：自动删除 Ground 再撤销，用于 CI
    float auto_close_seconds = 0.0f; // --auto-close N：运行 N 秒后自动关闭，用于 CI/关机测试
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--vulkan") == 0) {
            selected_api = render::RenderAPI::Vulkan;
            api_override_by_cli = true;
        } else if (std::strcmp(argv[i], "--opengl") == 0) {
            selected_api = render::RenderAPI::OpenGL;
            api_override_by_cli = true;
        } else if (std::strcmp(argv[i], "--vulkan-validation") == 0) {
            vulkan_validation = true;
        } else if (std::strcmp(argv[i], "--test-play-mode") == 0) {
            test_play_mode = true;
        } else if (std::strcmp(argv[i], "--test-delete-undo") == 0) {
            test_delete_undo = true;
        } else if (std::strcmp(argv[i], "--auto-close") == 0 && i + 1 < argc) {
            auto_close_seconds = static_cast<float>(std::atof(argv[++i]));
        }
    }
    if (args.should_record() && auto_close_seconds <= 0.0f) {
        auto_close_seconds = args.record_seconds;
    }

    utils::glog_initialize();
    utils::GLog::instance().set_min_level(utils::LogLevel::Info);
    // 安装内存 sink：控制台输出不变，同时供 Console 面板读取
    utils::GLog::instance().set_logger(
        std::make_unique<utils::MemoryLogSink>(std::make_unique<utils::ConsoleLogger>()));

    // 设置项目根目录（自动从可执行文件位置向上查找；运行时可通过 File > Load Project 切换）
    std::filesystem::path project_root = find_project_root();
    resources::Project::instance().set_root(project_root.string());
    components::register_builtin_components();

    // 初始化编辑器资源数据库（为资源生成 GUID .meta 文件）
    editor::AssetDatabase::instance().scan(project_root);

    // 加载项目设置：命令行未指定后端时，使用项目设置中的默认后端。
    editor::ProjectSettings project_settings = editor::ProjectSettingsWindow::load(project_root.string());
    if (!api_override_by_cli) {
        selected_api = project_settings.render_api;
    }
    // DX11/DX12 为预留后端（WinNative，尚未实现）：回退到默认 Vulkan。
    if (selected_api == render::RenderAPI::DX11 || selected_api == render::RenderAPI::DX12) {
        GLOG_WARN("Render backend '{}' is reserved and not implemented yet, falling back to Vulkan",
                  editor::render_api_to_string(selected_api));
        selected_api = render::RenderAPI::Vulkan;
    }

    // 初始化 GLFW
    if (!platform::Window::init_sdk()) {
        GLOG_ERROR("Failed to initialize GLFW");
        return -1;
    }

    // 创建窗口：OpenGL 需要 context，Vulkan 用 NoApi
    platform::WindowContextType window_ctx = (selected_api == render::RenderAPI::Vulkan)
                                                 ? platform::WindowContextType::NoApi
                                                 : platform::WindowContextType::OpenGL;
    platform::Window window("Gryce Engine Editor", args.resolution_w, args.resolution_h,
                            platform::WindowMode::Windowed, window_ctx);
    if (!window.is_valid()) {
        GLOG_ERROR("Failed to create window");
        platform::Window::shutdown_sdk();
        return -1;
    }

    // 启动画面预判（与 splash 初始化条件一致）：启用时先把主窗口切换为
    // 无边框小窗并居中，splash 结束后再恢复编辑器窗口状态。
    // 启动页面素材位于引擎目录 editor/asset_manager/background/，与示例项目完全分离
    std::filesystem::path engine_root_for_splash;
    {
        std::filesystem::path exe_path;
#ifdef _WIN32
        wchar_t buffer[MAX_PATH];
        if (GetModuleFileNameW(nullptr, buffer, MAX_PATH) > 0) {
            exe_path = std::filesystem::path(buffer);
        }
#else
        exe_path = std::filesystem::canonical("/proc/self/exe");
#endif
        std::filesystem::path dir = exe_path.parent_path();
        for (int i = 0; i < 8 && !dir.empty(); ++i) {
            if (std::filesystem::exists(dir / "CMakeLists.txt") &&
                std::filesystem::is_directory(dir / "core")) {
                engine_root_for_splash = dir;
                break;
            }
            dir = dir.parent_path();
        }
    }
    const std::filesystem::path splash_assets_dir = engine_root_for_splash / "editor" / "assets" / "background";
    const bool splash_will_show =
        !(args.headless || auto_close_seconds > 0.0f || args.should_record() ||
          test_play_mode || test_delete_undo) &&
        (std::filesystem::exists(splash_assets_dir / "splash.jpg") ||
         std::filesystem::exists(splash_assets_dir / "splash.png") ||
         std::filesystem::exists(splash_assets_dir / "icon.png"));
    constexpr int k_splash_w = 560;
    constexpr int k_splash_h = 340;
    if (splash_will_show) {
        window.set_decorated(false);
        window.set_size(k_splash_w, k_splash_h);
        window.center_on_primary_monitor();
        window.focus_window();
    }
    // splash 结束 / 跳过 / 上传失败时恢复编辑器窗口状态
    // 必须恢复 set_decorated(true)，因为 Mica 需要非客户区才能渲染
    // FluentWindow_Init 会随后去掉 WS_CAPTION 但保留 WS_THICKFRAME
    auto restore_editor_window = [&]() {
        if (!splash_will_show) return;
        window.set_decorated(true);  // 恢复系统装饰（Mica 需要非客户区）
        window.set_size(args.resolution_w, args.resolution_h);
        window.center_on_primary_monitor();
        window.focus_window();
    };
    if (args.headless) {
        glfwHideWindow(window.native_handle());
    }
    if (selected_api == render::RenderAPI::OpenGL) {
        window.set_vsync(false);
    }

    const bool is_vulkan = (selected_api == render::RenderAPI::Vulkan);

    // 创建渲染上下文
    render::RenderContext render_ctx;
    render_ctx.set_validation_enabled(vulkan_validation);
    if (!render_ctx.init(window.native_handle(), selected_api)) {
        GLOG_ERROR("Failed to initialize render context");
        platform::Window::shutdown_sdk();
        return -1;
    }



    // 创建 2D 渲染器（OpenGL / Vulkan 各自后端）
    auto renderer2d = render_ctx.create_renderer2d();
    if (renderer2d) {
        renderer2d->init(&render_ctx);
    }

    // 初始化 ImGui（必须在 render_ctx.start() 之前，GL context 还在主线程）
    render::ImGuiRenderer imgui;
    auto imgui_backend = render_ctx.create_imgui_backend();
    imgui.init(window.native_handle(), std::move(imgui_backend));

    // 布局持久化：ini 写到项目根目录而不是工作目录/build 目录。
    // 使用普通 std::string 而不用 static const，以便 File > Load Project 时更新路径。
    std::string imgui_ini_path = (project_root / "editor_imgui.ini").string();
    ImGui::GetIO().IniFilename = imgui_ini_path.c_str();

    // 加载主题预设与字体（失败则使用默认深色主题）
    editor::EditorSettings editor_settings = editor::SettingsWindow::load(project_root.string());
    editor::ThemePreset& theme_preset = editor_settings.theme_preset;

    // 多语言本地化必须先加载，这样 apply_theme 加载字体时才能按语言合并 CJK 字体
    editor::Localization::instance().load(editor_settings.appliance.language, project_root.string());
    editor::Localization::instance().set_light_theme(theme_preset == editor::ThemePreset::Light);
    editor::apply_theme(theme_preset, editor_settings.theme);

    // 应用持久化的 VSync 设置
    render_ctx.set_swap_interval(editor_settings.editor.vsync ? 1 : 0);

    // -------------------------------------------------------------------
    // 启动画面（splash）：大背景铺满 + 居中图标 + 底部进度条。
    // 资源位于引擎目录 editor/asset_manager/background/（splash.jpg 优先，回退 splash.png；icon.png）。
    // 实现方式：render_ctx.start() 之前 Vulkan/GL context 本来就归主线程，
    // 因此在各初始化阶段之间由主线程同步驱动 backend->begin_frame/end_frame
    // 泵一帧 ImGui 启动画面；不提前启动渲染线程，也不需要 pause/resume。
    // headless / --auto-close / --record / CI 测试模式下跳过；
    // 图片缺失时告警并跳过，不影响启动。
    // -------------------------------------------------------------------
    const bool splash_mode_disabled = args.headless || auto_close_seconds > 0.0f ||
                                      args.should_record() || test_play_mode || test_delete_undo;
    const bool splash_has_bg = std::filesystem::exists(splash_assets_dir / "splash.jpg") ||
                               std::filesystem::exists(splash_assets_dir / "splash.png");
    const bool splash_has_icon = std::filesystem::exists(splash_assets_dir / "icon.png");
    bool splash_active = false;
    uint64_t splash_bg_id = 0, splash_icon_id = 0;
    render::RHITextureHandle splash_bg_tex, splash_icon_tex;
    std::chrono::steady_clock::time_point splash_start;

    auto pump_splash = [&](float progress) {
        if (!splash_active) return;
        window.poll_events();
        if (window.should_close()) {
            splash_active = false;
            return;
        }
        render::IRenderBackend* backend = render_ctx.backend();
        if (!backend) return;

        // 主线程同步渲染一帧（此时 context 归主线程，渲染线程尚未启动）
        backend->begin_frame();
        imgui.begin_frame();
        ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        if (splash_bg_id) {
            // 大背景铺满（拉伸到全窗口）
            draw_list->AddImage(ImTextureRef(static_cast<ImTextureID>(splash_bg_id)),
                                ImVec2(0.0f, 0.0f), display);
        } else {
            draw_list->AddRectFilled(ImVec2(0.0f, 0.0f), display, IM_COL32(22, 23, 27, 255));
        }
        if (splash_icon_id) {
            // 图标居中（略偏上），边长取窗口高度 18%
            const float icon_size = display.y * 0.18f;
            const float cx = display.x * 0.5f;
            const float cy = display.y * 0.46f;
            draw_list->AddImage(ImTextureRef(static_cast<ImTextureID>(splash_icon_id)),
                                ImVec2(cx - icon_size * 0.5f, cy - icon_size * 0.5f),
                                ImVec2(cx + icon_size * 0.5f, cy + icon_size * 0.5f));
        }
        // 底部进度条（Xcode 蓝）
        const float bar_h = 4.0f;
        const float bar_y = display.y - bar_h - 2.0f;
        const float p = std::clamp(progress, 0.0f, 1.0f);
        draw_list->AddRectFilled(ImVec2(0.0f, bar_y), ImVec2(display.x, bar_y + bar_h),
                                 IM_COL32(0x3A, 0x3A, 0x3C, 255), 2.0f);
        if (p > 0.0f) {
            draw_list->AddRectFilled(ImVec2(0.0f, bar_y), ImVec2(display.x * p, bar_y + bar_h),
                                     IM_COL32(0x0A, 0x84, 0xFF, 255), 2.0f);
        }

        // 自定义关闭按钮（无边框窗口没有 OS 关闭按钮）：
        // 右上角小圆 + ×，悬停显示浅色圆底；点击按正常关闭流程退出
        const ImGuiIO& io = ImGui::GetIO();
        const float btn_r = 11.0f;
        const ImVec2 btn_c(display.x - btn_r - 8.0f, btn_r + 8.0f);
        const float mdx = io.MousePos.x - btn_c.x;
        const float mdy = io.MousePos.y - btn_c.y;
        const bool btn_hovered = (mdx * mdx + mdy * mdy) <= btn_r * btn_r;
        if (btn_hovered) {
            draw_list->AddCircleFilled(btn_c, btn_r, IM_COL32(255, 255, 255, 28));
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                window.request_close(); // 下一次 pump 停用 splash；主循环随即按关闭流程退出
            }
        }
        const float arm = btn_r * 0.42f;
        const ImU32 x_col = IM_COL32(255, 255, 255, btn_hovered ? 235 : 150);
        draw_list->AddLine(ImVec2(btn_c.x - arm, btn_c.y - arm),
                           ImVec2(btn_c.x + arm, btn_c.y + arm), x_col, 1.2f);
        draw_list->AddLine(ImVec2(btn_c.x + arm, btn_c.y - arm),
                           ImVec2(btn_c.x - arm, btn_c.y + arm), x_col, 1.2f);

        ImGui::Render();
        imgui.render_draw_data(ImGui::GetDrawData());
        backend->end_frame();
    };

    // 异步加载启动页素材的结构
    struct SplashAssetLoadResult {
        std::vector<uint8_t> bg_data;  // 背景图原始数据
        std::vector<uint8_t> icon_data; // 图标原始数据
        bool bg_loaded = false;
        bool icon_loaded = false;
    };

    if (!splash_mode_disabled && (splash_has_bg || splash_has_icon)) {
        GLOG_INFO("Splash: loading assets from '{}'", splash_assets_dir.string());
        splash_active = true;
        splash_start = std::chrono::steady_clock::now();

        // 先立即渲染一帧（纯色背景+进度条+关闭按钮），避免白屏
        pump_splash(0.05f);

        // 异步加载素材文件（读取到内存，不上传 GPU）
        std::future<SplashAssetLoadResult> asset_future = std::async(std::launch::async, [&]() {
            SplashAssetLoadResult result;
            if (splash_has_bg) {
                const std::filesystem::path bg_file =
                    std::filesystem::exists(splash_assets_dir / "splash.jpg")
                        ? splash_assets_dir / "splash.jpg"
                        : splash_assets_dir / "splash.png";
                GLOG_INFO("Splash: async loading background '{}'", bg_file.string());
                std::ifstream ifs(bg_file, std::ios::binary);
                if (ifs) {
                    result.bg_data = std::vector<uint8_t>((std::istreambuf_iterator<char>(ifs)),
                                                           std::istreambuf_iterator<char>());
                    result.bg_loaded = !result.bg_data.empty();
                }
            }
            if (splash_has_icon) {
                const std::filesystem::path icon_file = splash_assets_dir / "icon.png";
                GLOG_INFO("Splash: async loading icon '{}'", icon_file.string());
                std::ifstream ifs(icon_file, std::ios::binary);
                if (ifs) {
                    result.icon_data = std::vector<uint8_t>((std::istreambuf_iterator<char>(ifs)),
                                                             std::istreambuf_iterator<char>());
                    result.icon_loaded = !result.icon_data.empty();
                }
            }
            return result;
        });

        // 等待异步加载完成，期间持续渲染启动页
        while (asset_future.wait_for(std::chrono::milliseconds(16)) != std::future_status::ready) {
            pump_splash(0.1f);
            if (!splash_active) break;
        }

        if (!splash_active) {
            // 用户关闭了窗口
            restore_editor_window();
        } else {
            // 素材加载完成，上传到 GPU（需要主线程）
            SplashAssetLoadResult assets = asset_future.get();

            auto upload_texture = [&](const std::vector<uint8_t>& data,
                                      render::RHITextureHandle& handle) -> uint64_t {
                if (data.empty()) return 0;
                handle = render_ctx.create_texture();
                render::ITexture* tex = render_ctx.texture(handle);
                if (!tex) return 0;
                // 使用内存数据加载
                if (!tex->load_from_memory(data.data(), data.size())) {
                    return 0;
                }
                return imgui.backend() ? imgui.backend()->imgui_texture_id(tex) : 0;
            };

            if (assets.bg_loaded) {
                splash_bg_id = upload_texture(assets.bg_data, splash_bg_tex);
            }
            if (assets.icon_loaded) {
                splash_icon_id = upload_texture(assets.icon_data, splash_icon_tex);
            }

            if (splash_bg_id || splash_icon_id) {
                pump_splash(0.3f);
            } else {
                GLOG_WARN("Splash: assets loaded but failed to upload, skipping splash");
                restore_editor_window();
                splash_active = false;
            }
        }
    } else if (!splash_mode_disabled) {
        GLOG_WARN("Splash: no images found in '{}', skipping splash", splash_assets_dir.string());
    }

    // File > Load Project 时重新加载编辑器配置（主题、语言、字体）
    auto reload_editor_config = [&](const std::string& root) {
        editor_settings = editor::SettingsWindow::load(root);
        theme_preset = editor_settings.theme_preset;
        editor::Localization::instance().load(editor_settings.appliance.language, root);
        editor::Localization::instance().set_light_theme(theme_preset == editor::ThemePreset::Light);
        editor::apply_theme(theme_preset, editor_settings.theme);
        render_ctx.set_swap_interval(editor_settings.editor.vsync ? 1 : 0);
    };

    // 设置窗口
    editor::SettingsWindow settings_window;
    settings_window.set_render_context(&render_ctx);
    settings_window.set_rebuild_fonts_callback([&imgui]() {
        if (imgui.backend()) imgui.backend()->rebuild_fonts();
    });
    editor::ProjectSettingsWindow project_settings_window;
    editor::GImportEditorWindow gimport_editor_window;

    // 编辑器面板框架
    PanelManager panel_manager;
    auto* hierarchy_panel = panel_manager.add_panel<HierarchyPanel>();
    auto* inspector_panel = panel_manager.add_panel<InspectorPanel>();
    auto* viewport_panel = panel_manager.add_panel<ViewportPanel>();
    auto* game_view_panel = panel_manager.add_panel<GameViewPanel>();
    panel_manager.add_panel<ConsolePanel>();
    auto* project_panel = panel_manager.add_panel<FileExplorerPanel>();
    viewport_panel->set_imgui_backend(imgui.backend());
    game_view_panel->set_imgui_backend(imgui.backend());

    // Undo/Redo 命令栈与快捷键管理
    CommandStack undo_stack;
    ShortcutManager shortcuts;
    shortcuts.register_shortcut("Undo", {ImGuiKey_Z, true, false, false},
                                [&]() { undo_stack.undo(); });
    shortcuts.register_shortcut("Redo", {ImGuiKey_Y, true, false, false},
                                [&]() { undo_stack.redo(); });
    hierarchy_panel->set_undo_stack(&undo_stack);
    inspector_panel->set_undo_stack(&undo_stack);
    project_panel->set_undo_stack(&undo_stack);
    inspector_panel->set_render_context(&render_ctx);
    // Inspector「Add Component」按钮复用 Hierarchy 的组件选择器（同一 Undo 感知流程）
    inspector_panel->set_add_component_handler(
        [hierarchy_panel](scene::Entity* entity) { hierarchy_panel->open_component_picker(entity); });

    // Play Mode（M1-E4）：运行时预览，退出时从快照恢复场景
    bool play_mode_active = false;
    std::string play_mode_snapshot;
    viewport_panel->set_editing_enabled(!play_mode_active);

    // 输入（仅保留 F1 线框调试开关；相机控制走 EditorCamera / ImGui IO）
    platform::InputManager input;
    input.update(&window);

    // 编辑器自由飞行相机（不依赖场景相机实体）
    EditorCamera editor_camera;
    math::Camera& camera = editor_camera.camera();

    // Hierarchy 聚焦回调：将编辑器相机对准选中实体
    hierarchy_panel->set_focus_handler([&](scene::Entity* e) {
        if (!e) return;
        math::Vector3f center;
        float radius = 0.0f;
        if (compute_entity_world_bounds(e, center, radius)) {
            editor_camera.focus_on_bounds(center, radius);
        } else if (e->transform()) {
            editor_camera.focus_on(e->transform()->position);
        }
    });

    // Game View 独立相机：从场景主摄像机构建
    math::Camera game_camera;

    // 视口面板接线：相机 + 选中实体（Hierarchy UUID 弱引用解析）
    viewport_panel->set_camera(&camera);
    viewport_panel->set_selection_provider(
        [hierarchy_panel]() { return hierarchy_panel->selected_entity(); });
    viewport_panel->set_undo_stack(&undo_stack);

    pump_splash(0.45f);

    // 加载或创建场景（必须在 render_ctx.start() 之前，因为上传网格需要主线程 GL context）
    std::string scene_path = "res:/scenes/main.gesc";
    std::unique_ptr<scene::Scene> current_scene = scene::SceneSerializer::load_from_file(scene_path);
    if (!current_scene) {
        GLOG_INFO("No existing scene found at '{}', creating demo scene", scene_path);
        current_scene = create_demo_scene(1920.0f, 1080.0f);
        scene::SceneSerializer::save_to_file(*current_scene, scene_path);
    } else {
        GLOG_INFO("Scene loaded from '{}'", scene_path);
    }

    // 缓存 FPS Label 组件指针，便于每帧更新文字
    components::d2::text::Label* fps_label = nullptr;
    if (current_scene) {
        scene::Entity* fps_entity = current_scene->find_entity_by_name("FPS_Label");
        if (fps_entity) {
            fps_label = fps_entity->get_component<components::d2::text::Label>();
        }

        // 确保物理演示场景有地面和可下落的 cube
        ensure_physics_demo_entities(*current_scene);

        // 预上传所有 3D 网格到 GPU（必须在 start() 之前，主线程持有 GL context）
        upload_scene_meshes(*current_scene, render_ctx);

        // 确保场景有主摄像机和主光源
        ensure_scene_defaults(*current_scene, camera);
        sync_editor_to_scene_camera(*current_scene, camera);
    }

    // 命令行指定了场景时，加载该场景（支持 res:/ 路径、绝对路径、相对项目根路径）。
    if (!args.scene_path.empty()) {
        std::string cli_scene = resolve_scene_path(args.scene_path, project_root);
        auto cli_loaded = scene::SceneSerializer::load_from_file(cli_scene);
        if (cli_loaded) {
            current_scene = std::move(cli_loaded);
            scene_path = cli_scene;
            GLOG_INFO("CLI scene loaded from '{}'", cli_scene);

            // 重新执行上传与默认对象保证
            if (current_scene) {
                scene::Entity* fps_entity = current_scene->find_entity_by_name("FPS_Label");
                fps_label = fps_entity ? fps_entity->get_component<components::d2::text::Label>() : nullptr;
                ensure_physics_demo_entities(*current_scene);
                upload_scene_meshes(*current_scene, render_ctx);
                ensure_scene_defaults(*current_scene, camera);
                sync_editor_to_scene_camera(*current_scene, camera);
            }
        } else {
            GLOG_ERROR("Failed to load CLI scene '{}', falling back to default", cli_scene);
        }
    }

    // 应用相机预设
    editor_camera.set_preset(args.camera_preset);
    if (args.camera_preset == CameraPreset::Orbit) {
        // 轨道中心取场景包围盒中心；无实体时取原点。
        math::Vector3f center = math::Vector3f::zero();
        float radius = 5.0f;
        if (current_scene) {
            compute_scene_bounds(*current_scene, center, radius);
        }
        editor_camera.set_orbit_target(center);
        editor_camera.set_orbit_radius(std::max(radius * 1.5f, 5.0f));
    } else if (args.camera_preset == CameraPreset::Static && current_scene) {
        // Static 固定最佳视角：聚焦场景包围盒。
        math::Vector3f center;
        float radius = 0.0f;
        if (compute_scene_bounds(*current_scene, center, radius)) {
            editor_camera.focus_on_bounds(center, radius);
        }
    }

    // 场景文件热重载：记录最后修改时间
    std::filesystem::file_time_type scene_last_write = get_scene_write_time(scene_path);
    double scene_reload_timer = 0.0;

    // 创建渲染管线（必须在 start() 之前，主线程持有 GL context）。
    // 视口离屏输出：tonemap 结果写入独立 FBO 供 Viewport / Game View 面板采样。
    // 项目设置中的渲染质量选项（阴影/HDR/环境光等）在 init 前应用到两条管线。
    auto apply_quality_settings = [&project_settings](render::RenderPipeline& p) {
        const auto& q = project_settings.quality;
        p.set_shadow_map_size(q.shadow_map_size);
        p.set_shadow_bias(q.shadow_bias);
        p.set_shadow_area(q.shadow_area);
        p.set_ambient(math::Vector3f(q.ambient[0], q.ambient[1], q.ambient[2]));
        p.set_hdr_enabled(q.hdr_enabled);
        p.set_exposure(q.exposure);
        p.set_tone_map_mode(q.tone_map_mode);
        p.set_ibl_intensity(q.ibl_intensity);
    };
    render::RenderPipeline pipeline;
    pipeline.set_viewport_output_enabled(true);
    pipeline.set_imgui_backend(imgui.backend());
    apply_quality_settings(pipeline);
    if (!pipeline.init(&render_ctx, "res:/shaders")) {
        GLOG_ERROR("Failed to initialize render pipeline");
        render_ctx.shutdown();
        platform::Window::shutdown_sdk();
        return -1;
    }
    viewport_panel->set_pipeline(&pipeline);

    // Game View 独立渲染管线：使用场景主摄像机，渲染到独立 FBO
    render::RenderPipeline game_pipeline;
    game_pipeline.set_viewport_output_enabled(true);
    game_pipeline.set_imgui_backend(imgui.backend());
    apply_quality_settings(game_pipeline);
    if (!game_pipeline.init(&render_ctx, "res:/shaders")) {
        GLOG_WARN("Failed to initialize game view pipeline; Game View will be unavailable");
    }
    game_view_panel->set_pipeline(&game_pipeline);

    pump_splash(0.8f);

#ifdef _WIN32
    // TEMP-DEBUG: verify borderless style + capture splash frame
    {
        HWND hwnd = glfwGetWin32Window(window.native_handle());
        LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
        GLOG_INFO("TEMP splash window style=0x{:x} WS_CAPTION={} size={}x{}", style,
                  (style & WS_CAPTION) ? 1 : 0, k_splash_w, k_splash_h);
        std::vector<uint8_t> rgba; int cw = 0, ch = 0;
        if (render_ctx.capture_frame_rgba(rgba, cw, ch)) {
            stbi_write_png("D:/tmp/splash_dump2.png", cw, ch, 4, rgba.data(), cw * 4);
        }
    }
#endif

    // 窗口大小变化时先记录尺寸，主循环中安全同步渲染目标
    struct WindowResizeState {
        std::atomic<int> width{0};
        std::atomic<int> height{0};
        std::atomic<bool> pending{false};
    } window_resize_state;

    window.set_resize_callback([&](int w, int h) {
        if (w <= 0 || h <= 0) return;
        render_ctx.set_viewport(0, 0, w, h);
        window_resize_state.width.store(w);
        window_resize_state.height.store(h);
        window_resize_state.pending.store(true);
    });

    // 创建 ECS World 并注册系统
    ecs::World world;
    if (current_scene) {
        world.attach_scene(std::move(current_scene));
        world.add_system<ecs::PhysicsSystem3D>();
        world.add_system<ecs::RenderSystem3D>(&pipeline);
        if (renderer2d) {
            world.add_system<ecs::RenderSystem2D>(renderer2d.get());
        }
        world.init();
        inspector_panel->set_scene(world.scene());
    }

    // 组件增删后即时热重载物理 body（不等待下一帧）
    auto component_changed_handler = [&](scene::Entity* entity) {
        if (!entity) return;
        if (auto* ps = world.get_system<ecs::PhysicsSystem3D>()) {
            ps->rebuild_body_for_entity(entity);
        }
    };
    inspector_panel->set_component_changed_handler(component_changed_handler);
    hierarchy_panel->set_component_changed_handler(component_changed_handler);

    pump_splash(0.9f);

    // -------------------------------------------------------------------
    // File 菜单：场景保存 / 加载（M1-E2）
    // -------------------------------------------------------------------
    bool save_as_popup_requested = false;
    bool open_popup_requested = false;
    bool load_project_popup_requested = false;
    char save_as_buf[256] = "res:/scenes/main.gesc";
    char open_buf[256] = "res:/scenes/main.gesc";
    char load_project_buf[512] = "";

    // FPS Label 组件指针在场景替换后需要重新查找
    auto refresh_fps_label = [&]() {
        fps_label = nullptr;
        if (world.scene()) {
            scene::Entity* fps_entity = world.scene()->find_entity_by_name("FPS_Label");
            if (fps_entity) {
                fps_label = fps_entity->get_component<components::d2::text::Label>();
            }
        }
    };

    auto save_scene = [&](const std::string& path) {
        if (!world.scene()) return;
        if (scene::SceneSerializer::save_to_file(*world.scene(), path)) {
            scene_path = path;
            // 保存会改写文件 mtime，刷新缓存避免紧接着触发一次热重载
            scene_last_write = get_scene_write_time(scene_path);
            world.scene()->mark_saved();
            GLOG_INFO("Scene saved to '{}'", path);
        } else {
            GLOG_ERROR("Failed to save scene to '{}'", path);
        }
    };

    auto restore_scene_from_snapshot = [&](const std::string& snapshot_json) {
        nlohmann::json json;
        try {
            json = nlohmann::json::parse(snapshot_json);
        } catch (const std::exception& e) {
            GLOG_ERROR("Play Mode: failed to parse scene snapshot: {}", e.what());
            return;
        }
        auto loaded = scene::SceneSerializer::deserialize(json);
        if (!loaded) {
            GLOG_ERROR("Play Mode: failed to deserialize scene snapshot");
            return;
        }
        world.detach_scene();
        hierarchy_panel->clear_selection();
        render_ctx.pause_render_thread();
        ensure_physics_demo_entities(*loaded);
        upload_scene_meshes(*loaded, render_ctx);
        ensure_scene_defaults(*loaded, camera);
        sync_editor_to_scene_camera(*loaded, camera);
        world.attach_scene(std::move(loaded));
        render_ctx.resume_render_thread();

        refresh_fps_label();
        GLOG_INFO("Play Mode: scene restored from snapshot");
    };

    auto exit_play_mode = [&]() {
        if (!play_mode_active) return;
        play_mode_active = false;
        viewport_panel->set_editing_enabled(true);
        hierarchy_panel->set_drag_enabled(true);
        inspector_panel->set_read_only(false);
        if (!play_mode_snapshot.empty()) {
            restore_scene_from_snapshot(play_mode_snapshot);
            play_mode_snapshot.clear();
        }
        ImGui::SetWindowFocus("Viewport");
        GLOG_INFO("Play Mode: exited");
    };

    auto enter_play_mode = [&]() {
        if (!world.scene() || play_mode_active) return;
        play_mode_snapshot = scene::SceneSerializer::serialize(*world.scene()).dump();
        play_mode_active = true;
        viewport_panel->set_editing_enabled(false);
        hierarchy_panel->set_drag_enabled(false);
        inspector_panel->set_read_only(true);
        ImGui::SetWindowFocus("Game");
        GLOG_INFO("Play Mode: entered");
    };

    auto toggle_play_mode = [&]() {
        if (play_mode_active) {
            exit_play_mode();
        } else {
            enter_play_mode();
        }
    };

    // 注册全局快捷键（Ctrl+Z/Y 已在前面注册）
    shortcuts.register_shortcut("Save Scene", {ImGuiKey_S, true, false, false},
                                [&]() { save_scene(scene_path); });
    shortcuts.register_shortcut("Toggle Play Mode", {ImGuiKey_P, true, false, false},
                                [&]() { toggle_play_mode(); });
    shortcuts.register_shortcut("Delete Entity", {ImGuiKey_Delete, false, false, false}, [&]() {
        if (auto* e = hierarchy_panel->selected_entity()) {
            hierarchy_panel->queue_delete(e->uuid());
        }
    });
    shortcuts.register_shortcut("Cut Entity", {ImGuiKey_X, true, false, false}, [&]() {
        hierarchy_panel->cut_entity(hierarchy_panel->selected_entity());
    });
    shortcuts.register_shortcut("Copy Entity", {ImGuiKey_C, true, false, false}, [&]() {
        hierarchy_panel->copy_entity(hierarchy_panel->selected_entity());
    });
    shortcuts.register_shortcut("Paste Entity", {ImGuiKey_V, true, false, false}, [&]() {
        hierarchy_panel->paste_clipboard(hierarchy_panel->selected_entity());
    });
    shortcuts.register_shortcut("Duplicate Entity", {ImGuiKey_D, true, false, false}, [&]() {
        hierarchy_panel->duplicate_entity(hierarchy_panel->selected_entity());
    });
    shortcuts.register_shortcut("Rename Entity", {ImGuiKey_F2, false, false, false}, [&]() {
        hierarchy_panel->rename_selected();
    });
    shortcuts.register_shortcut("Focus Selected", {ImGuiKey_F, false, false, false}, [&]() {
        if (!viewport_panel->hovered()) return;
        auto* e = hierarchy_panel->selected_entity();
        if (!e) return;

        math::Vector3f center;
        float radius = 0.0f;
        if (compute_entity_world_bounds(e, center, radius)) {
            editor_camera.focus_on_bounds(center, radius);
        } else if (e->transform()) {
            editor_camera.focus_on(e->transform()->position);
        }
    });

    // 应用持久化的快捷键覆盖（editor_settings.json "shortcuts" 组）
    for (const auto& [name, combo_str] : editor_settings.shortcut_overrides) {
        ShortcutManager::KeyCombo combo;
        if (ShortcutManager::combo_from_string(combo_str, combo)) {
            if (!shortcuts.set_combo(name, combo)) {
                GLOG_WARN("Shortcuts: unknown shortcut '{}', ignored", name);
            }
        }
    }
    settings_window.set_shortcut_manager(&shortcuts);

    auto open_scene = [&](const std::string& path) {
        GLOG_INFO("Open Scene: starting '{}'", path);
        // 打开新场景前若处于 Play Mode，先退出并丢弃快照
        if (play_mode_active) {
            GLOG_INFO("Open Scene: exiting play mode");
            exit_play_mode();
        }

        GLOG_INFO("Open Scene: loading from file");
        auto loaded = scene::SceneSerializer::load_from_file(path);
        if (!loaded) {
            GLOG_ERROR("Failed to open scene from '{}'", path);
            return;
        }
        // 与热重载相同的替换流程：暂停渲染线程后上传 GPU 资源
        GLOG_INFO("Open Scene: detaching old scene");
        world.detach_scene();
        hierarchy_panel->clear_selection();
        GLOG_INFO("Open Scene: pausing render thread");
        render_ctx.pause_render_thread();
        GLOG_INFO("Open Scene: ensuring physics entities");
        ensure_physics_demo_entities(*loaded);
        GLOG_INFO("Open Scene: uploading meshes");
        upload_scene_meshes(*loaded, render_ctx);
        GLOG_INFO("Open Scene: ensuring defaults");
        ensure_scene_defaults(*loaded, camera);
        GLOG_INFO("Open Scene: syncing camera");
        sync_editor_to_scene_camera(*loaded, camera);
        GLOG_INFO("Open Scene: attaching new scene");
        world.attach_scene(std::move(loaded));

        // 加载新场景后重建渲染管线，确保 shader / 目标与当前项目一致
        GLOG_INFO("Open Scene: rebuilding render pipelines");
        apply_quality_settings(pipeline);
        pipeline.rebuild(&render_ctx, "res:/shaders");
        if (game_pipeline.is_valid()) {
            apply_quality_settings(game_pipeline);
            game_pipeline.rebuild(&render_ctx, "res:/shaders");
        }

        GLOG_INFO("Open Scene: resuming render thread");
        render_ctx.resume_render_thread();

        scene_path = path;
        scene_last_write = get_scene_write_time(scene_path);
        refresh_fps_label();
        GLOG_INFO("Scene opened from '{}'", path);
    };

    // -----------------------------------------------------------------------
    // File > Load Project：切换工作项目
    // -----------------------------------------------------------------------
    auto load_project = [&](const std::string& path) {
        GLOG_INFO("Load Project: starting '{}'", path);
        std::filesystem::path new_root(path);
        if (new_root.empty() || !std::filesystem::is_directory(new_root)) {
            GLOG_ERROR("Load Project: '{}' is not a valid directory", path);
            return;
        }
        if (!std::filesystem::exists(new_root / "project.gryce")) {
            GLOG_ERROR("Load Project: '{}' does not contain project.gryce", path);
            return;
        }
        try {
            new_root = std::filesystem::canonical(new_root);
        } catch (const std::exception& e) {
            GLOG_ERROR("Load Project: failed to canonicalize '{}': {}", path, e.what());
            return;
        }
        GLOG_INFO("Load Project: validated root '{}'", new_root.string());

        // 保存当前场景并退出 Play Mode
        if (world.scene()) {
            GLOG_INFO("Load Project: saving current scene");
            save_scene(scene_path);
        }
        if (play_mode_active) {
            GLOG_INFO("Load Project: exiting play mode");
            exit_play_mode();
        }

        // 更新项目上下文
        GLOG_INFO("Load Project: updating project root");
        project_root = new_root;
        resources::Project::instance().set_root(project_root.string());
        GLOG_INFO("Load Project: scanning asset database");
        editor::AssetDatabase::instance().scan(project_root);

        // 重新加载项目与编辑器配置
        GLOG_INFO("Load Project: reloading project settings");
        project_settings = editor::ProjectSettingsWindow::load(project_root.string());
        GLOG_INFO("Load Project: reloading editor config");
        reload_editor_config(project_root.string());

        // 更新 ImGui ini 路径
        GLOG_INFO("Load Project: updating imgui ini path");
        imgui_ini_path = (project_root / "editor_imgui.ini").string();
        ImGui::GetIO().IniFilename = imgui_ini_path.c_str();

        // 加载新项目默认场景
        GLOG_INFO("Load Project: opening default scene");
        scene_path = "res:/scenes/main.gesc";
        open_scene(scene_path);

        GLOG_INFO("Project loaded from '{}'", project_root.string());
    };

    // -----------------------------------------------------------------------
    // 资源浏览器（Project 面板）回调：双击 / 拖拽
    // -----------------------------------------------------------------------
    auto to_lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    auto extension_of = [&](const std::string& path) {
        return to_lower(std::filesystem::path(path).extension().string());
    };
    auto is_scene_file = [&](const std::string& path) {
        return extension_of(path) == ".gesc";
    };
    auto is_mesh_file = [&](const std::string& path) {
        const std::string ext = extension_of(path);
        return ext == ".obj" || ext == ".fbx" || ext == ".gltf" || ext == ".glb";
    };
    auto is_texture_file = [&](const std::string& path) {
        const std::string ext = extension_of(path);
        return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" ||
               ext == ".tga" || ext == ".dds" || ext == ".ktx" || ext == ".hdr";
    };
    auto is_gimport_file = [&](const std::string& path) {
        return extension_of(path) == ".gimport";
    };
    auto is_prefab_file = [&](const std::string& path) {
        const std::string ext = extension_of(path);
        return ext == ".geprefab" || ext == ".geprefabvariant";
    };
    auto file_stem = [&](const std::string& path) {
        return std::filesystem::path(path).stem().string();
    };

    auto apply_texture_to_material = [&](components::MeshRenderer* mr, const std::string& tex_path) {
        if (!mr || !mr->material) return;
        render::Material* mat = mr->material.get();

        const std::string lower = to_lower(file_stem(tex_path));
        if (lower.find("normal") != std::string::npos) {
            mat->normal_map_path = tex_path;
            mat->use_normal_map = true;
        } else if (lower.find("roughness") != std::string::npos) {
            mat->roughness_map_path = tex_path;
            mat->use_roughness_map = true;
        } else if (lower.find("metallic") != std::string::npos || lower.find("metal") != std::string::npos) {
            mat->metallic_map_path = tex_path;
            mat->use_metallic_map = true;
        } else if (lower.find("ao") != std::string::npos || lower.find("ambient") != std::string::npos) {
            mat->ao_map_path = tex_path;
            mat->use_ao_map = true;
        } else {
            mat->albedo_map_path = tex_path;
            mat->use_albedo_map = true;
        }

        // 若网格已上传，材质也已在 GPU，需要异步销毁旧纹理并重新上传新材质
        if (mr->gpu_mesh()) {
            auto token = mr->alive_token();
            render_ctx.push_command([mat, ctx = &render_ctx, token](render::IRenderBackend*) {
                if (!token || !token->load(std::memory_order_acquire)) return;
                mat->destroy_gpu(ctx);
                mat->upload_to_gpu(ctx);
            });
        }
    };

    auto instantiate_mesh_entity = [&](scene::Entity* parent, const std::string& mesh_path) -> scene::Entity* {
        scene::Scene* scene = world.scene();
        if (!scene) return nullptr;

        const std::string name = file_stem(mesh_path);
        scene::Entity* entity = nullptr;
        if (parent) {
            auto child = std::make_unique<scene::Entity>(name);
            entity = parent->add_child(std::move(child));
        } else {
            entity = scene->create_entity(name);
        }
        if (!entity) return nullptr;

        editor::GImportSettings import_settings = editor::ensure_gimport_settings(mesh_path);

        auto* mr = entity->add_component<components::MeshRenderer>(mesh_path);
        if (mr && mr->material) {
            mr->material->name = name;
        }

        entity->transform()->scale = math::Vector3f::one() * import_settings.scale;

        if (import_settings.add_rigidbody) {
            auto* rb = entity->add_component<components::RigidBody>();
            if (rb && !import_settings.physics_material.empty()) {
                if (auto* pm = entity->add_component<components::PhysicalMaterial>()) {
                    pm->apply_preset(import_settings.physics_material);
                }
            }
        }

        hierarchy_panel->select(entity->uuid());
        GLOG_INFO("Project: instantiated mesh '{}' as entity '{}' (scale={:.2f}, collider={}, rigidbody={})",
                  mesh_path, name, import_settings.scale,
                  import_settings.generate_collider, import_settings.add_rigidbody);
        return entity;
    };

    auto create_textured_cube = [&](scene::Entity* parent, const std::string& tex_path) -> scene::Entity* {
        scene::Entity* entity = instantiate_mesh_entity(parent, "res:/models/cube_pbr.obj");
        if (!entity) return nullptr;
        auto* mr = entity->get_component<components::MeshRenderer>();
        if (mr && mr->material) {
            mr->material->name = file_stem(tex_path);
            apply_texture_to_material(mr, tex_path);
        }
        return entity;
    };

    auto instantiate_prefab_entity = [&](scene::Entity* parent, const std::string& prefab_path) -> scene::Entity* {
        scene::Scene* scene = world.scene();
        if (!scene) return nullptr;

        auto tree = scene::Prefab::instantiate_tree(prefab_path);
        if (!tree) {
            GLOG_ERROR("Project: failed to instantiate prefab '{}'", prefab_path);
            return nullptr;
        }

        scene::Entity* root = tree.get();
        if (parent) {
            parent->add_child(std::move(tree));
        } else {
            scene->add_root_entity(std::move(tree));
        }

        hierarchy_panel->select(root->uuid());
        GLOG_INFO("Project: instantiated prefab '{}' as entity '{}'", prefab_path, root->name());
        return root;
    };

    auto apply_file_to_entity = [&](scene::Entity* entity, const std::string& path) {
        if (!entity) return;
        if (is_mesh_file(path)) {
            // 替换为新的 MeshRenderer，渲染线程会自动上传
            if (auto* old = entity->get_component<components::MeshRenderer>()) {
                entity->remove_component(old);
            }
            auto* mr = entity->add_component<components::MeshRenderer>(path);
            if (mr && mr->material) {
                mr->material->name = file_stem(path);
            }
            GLOG_INFO("Project: set mesh '{}' on entity '{}'", path, entity->name());
        } else if (is_texture_file(path)) {
            auto* mr = entity->get_component<components::MeshRenderer>();
            if (!mr) {
                mr = entity->add_component<components::MeshRenderer>("res:/models/cube_pbr.obj");
                if (mr && mr->material) mr->material->name = "TexturedCube";
            }
            if (mr) {
                apply_texture_to_material(mr, path);
                GLOG_INFO("Project: set texture '{}' on entity '{}'", path, entity->name());
            }
        } else {
            GLOG_WARN("Project: dropping '{}' onto Inspector is not supported", path);
        }
    };

    auto handle_activate_file = [&](const std::string& path) {
        if (play_mode_active) {
            GLOG_WARN("Project: cannot activate resources while Play Mode is active");
            return;
        }
        if (is_scene_file(path)) {
            open_scene(path);
        } else if (is_mesh_file(path)) {
            instantiate_mesh_entity(nullptr, path);
        } else if (is_texture_file(path)) {
            create_textured_cube(nullptr, path);
        } else if (is_gimport_file(path)) {
            // .gimport 文件记录的是对应源资源的导入设置
            std::string source_path = std::filesystem::path(path).replace_extension().string();
            // 若源资源路径带有两层扩展名（如 .obj.gimport），需要进一步还原
            if (std::filesystem::path(source_path).extension().empty()) {
                source_path = std::filesystem::path(source_path).replace_extension().string();
            }
            gimport_editor_window.open(source_path);
        } else if (is_prefab_file(path)) {
            instantiate_prefab_entity(nullptr, path);
        } else {
            GLOG_WARN("Project: double-click on '{}' is not supported", path);
        }
    };

    project_panel->on_activate_file = handle_activate_file;

    viewport_panel->set_drop_handler([&](const std::string& path) {
        if (play_mode_active) {
            GLOG_WARN("Project: cannot drop resources while Play Mode is active");
            return;
        }
        if (is_scene_file(path)) {
            open_scene(path);
        } else if (is_mesh_file(path)) {
            instantiate_mesh_entity(nullptr, path);
        } else if (is_texture_file(path)) {
            create_textured_cube(nullptr, path);
        } else if (is_prefab_file(path)) {
            instantiate_prefab_entity(nullptr, path);
        }
    });

    hierarchy_panel->set_drop_handler([&](scene::Entity* target, const std::string& path) {
        if (play_mode_active) {
            GLOG_WARN("Project: cannot drop resources while Play Mode is active");
            return;
        }
        if (is_scene_file(path)) {
            open_scene(path);
        } else if (is_mesh_file(path)) {
            instantiate_mesh_entity(target, path);
        } else if (is_texture_file(path)) {
            if (target) {
                apply_file_to_entity(target, path);
            } else {
                create_textured_cube(nullptr, path);
            }
        } else if (is_prefab_file(path)) {
            instantiate_prefab_entity(target, path);
        }
    });

    inspector_panel->set_drop_handler([&](scene::Entity* entity, const std::string& path) {
        if (play_mode_active) {
            GLOG_WARN("Project: cannot drop resources while Play Mode is active");
            return;
        }
        apply_file_to_entity(entity, path);
    });

    // 菜单栏钩子：注入到 FluentWindow 标题栏
    // 左侧 Logo 由 FluentWindow 绘制，右侧系统按钮也由 FluentWindow 处理
    editor::FluentWindow_SetMenuBarHook([&]() {
        if (ImGui::BeginMenu(editor::tr("menu.file"))) {
            if (ImGui::MenuItem(editor::tr("menu.load_project"))) {
                std::strncpy(load_project_buf, project_root.string().c_str(), sizeof(load_project_buf) - 1);
                load_project_buf[sizeof(load_project_buf) - 1] = '\0';
                load_project_popup_requested = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem(editor::tr("menu.save_scene"), "Ctrl+S")) {
                save_scene(scene_path);
            }
            if (ImGui::MenuItem(editor::tr("menu.save_scene_as"))) {
                std::strncpy(save_as_buf, scene_path.c_str(), sizeof(save_as_buf) - 1);
                save_as_buf[sizeof(save_as_buf) - 1] = '\0';
                save_as_popup_requested = true;
            }
            if (ImGui::MenuItem(editor::tr("menu.load_scene"))) {
                std::strncpy(open_buf, scene_path.c_str(), sizeof(open_buf) - 1);
                open_buf[sizeof(open_buf) - 1] = '\0';
                open_popup_requested = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem(editor::tr("menu.project_settings"))) {
                project_settings_window.open();
            }
            if (ImGui::MenuItem(editor::tr("menu.settings"))) {
                settings_window.open();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(editor::tr("menu.edit"))) {
            // Undo/Redo 与全局快捷键共用同一命令栈
            if (ImGui::MenuItem(editor::tr("menu.undo"), "Ctrl+Z", false, undo_stack.can_undo())) {
                undo_stack.undo();
            }
            if (ImGui::MenuItem(editor::tr("menu.redo"), "Ctrl+Y", false, undo_stack.can_redo())) {
                undo_stack.redo();
            }
            ImGui::Separator();
            scene::Entity* selected = hierarchy_panel->selected_entity();
            if (ImGui::MenuItem(editor::tr("menu.cut"), "Ctrl+X", false, selected != nullptr)) {
                hierarchy_panel->cut_entity(selected);
            }
            if (ImGui::MenuItem(editor::tr("menu.copy"), "Ctrl+C", false, selected != nullptr)) {
                hierarchy_panel->copy_entity(selected);
            }
            if (ImGui::MenuItem(editor::tr("menu.paste"), "Ctrl+V", false, hierarchy_panel->has_clipboard())) {
                hierarchy_panel->paste_clipboard(selected);
            }
            if (ImGui::MenuItem(editor::tr("menu.duplicate"), "Ctrl+D", false, selected != nullptr)) {
                hierarchy_panel->duplicate_entity(selected);
            }
            if (ImGui::MenuItem(editor::tr("menu.rename"), "F2", false, selected != nullptr)) {
                hierarchy_panel->rename_selected();
            }
            if (ImGui::MenuItem(editor::tr("menu.delete"), "Del", false, selected != nullptr)) {
                hierarchy_panel->queue_delete(selected->uuid());
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(editor::tr("menu.play"))) {
            if (ImGui::MenuItem(play_mode_active ? editor::tr("menu.play_stop") : editor::tr("menu.play_play"), "Ctrl+P")) {
                toggle_play_mode();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(editor::tr("menu.view"))) {
            // 主题详细设置统一放到 File > Settings；这里只保留明暗快速切换。
            bool is_dark = (theme_preset == editor::ThemePreset::Dark);
            if (ImGui::MenuItem(editor::tr("menu.view_theme_dark"), nullptr, &is_dark)) {
                render_ctx.pause_render_thread();
                theme_preset = editor::ThemePreset::Dark;
                editor::apply_theme(theme_preset, editor_settings.theme);
                if (imgui.backend()) imgui.backend()->rebuild_fonts();
                editor::Localization::instance().set_light_theme(false);
                editor::SettingsWindow::save(project_root.string(), editor_settings);
                render_ctx.resume_render_thread();
            }
            bool is_light = (theme_preset == editor::ThemePreset::Light);
            if (ImGui::MenuItem(editor::tr("menu.view_theme_light"), nullptr, &is_light)) {
                render_ctx.pause_render_thread();
                theme_preset = editor::ThemePreset::Light;
                editor::apply_theme(theme_preset, editor_settings.theme);
                if (imgui.backend()) imgui.backend()->rebuild_fonts();
                editor::Localization::instance().set_light_theme(true);
                editor::SettingsWindow::save(project_root.string(), editor_settings);
                render_ctx.resume_render_thread();
            }
            ImGui::EndMenu();
        }
    });

    // splash 收尾：满进度并保证最短显示 0.8s（避免一闪而过），随后销毁启动画面纹理
    if (splash_active) {
        pump_splash(0.98f);
        while (std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                             splash_start).count() < 0.8) {
            pump_splash(1.0f);
        }
        splash_active = false;
        render_ctx.destroy_texture(splash_bg_tex);
        render_ctx.destroy_texture(splash_icon_tex);
        // 恢复编辑器窗口状态：无边框 + 原始分辨率 + 居中聚焦
        restore_editor_window();
    }

    // 启动渲染线程（此后主线程不再持有 GL context）
    render_ctx.start();

#ifdef _WIN32
    // 初始化 FluentWindow（DWM Mica Alt + 圆角 + 自定义标题栏）
    // 必须在渲染线程启动后调用，因为 WndProc 子类化需要窗口已创建
    editor::FluentWindow_Init(window.native_handle());
    GLOG_INFO("FluentWindow initialized (DWM Mica Alt + rounded corners)");
#endif

    GLOG_INFO("Entering editor main loop...");
    GLOG_INFO("Viewport controls: RMB drag look | WASD+QE move (hold RMB) | Shift sprint | Wheel zoom | Close window to exit");

    utils::FrameLimiter frame_limiter;
    frame_limiter.set_target_fps(0); // 默认不限制帧率，让 GPU 全力跑

    bool wireframe_mode = false;
    double auto_close_timer = 0.0;
    double play_mode_test_timer = 0.0;
    bool play_mode_test_entered = false;
    double delete_undo_test_timer = 0.0;
    bool delete_test_done = false;
    bool undo_test_done = false;
    double autosave_timer = 0.0;
    int last_autosave_interval_min = editor_settings.editor.autosave_interval_min;

    // 录制器：命令行 --record 时创建
    std::unique_ptr<FrameRecorder> recorder;
    if (args.should_record()) {
        std::filesystem::path output = args.output_path.empty()
                                           ? std::filesystem::path("clip.mp4")
                                           : std::filesystem::path(args.output_path);
        recorder = std::make_unique<FrameRecorder>(output, 30, args.no_audio);
        GLOG_INFO("FrameRecorder initialized: output='{}' duration={}s", output.string(), args.record_seconds);
    }

    // 录制时隐藏 FPS 等调试 UI
    const bool hide_debug_ui = args.should_record();
    if (hide_debug_ui) {
        fps_label = nullptr;
    }

    // Viewport 面板尺寸 → 渲染目标尺寸：防抖 0.15s，避免拖动 dock 分隔条时
    // 每帧 pause/resume 渲染线程造成的卡顿。
    int pending_vw = 0, pending_vh = 0;
    int applied_vw = 0, applied_vh = 0;
    float viewport_resize_timer = 0.0f;
    constexpr float k_resize_debounce = 0.15f;

    // Game View 面板尺寸 → 独立渲染目标尺寸：同样做防抖
    int pending_gw = 0, pending_gh = 0;
    int applied_gw = 0, applied_gh = 0;
    float game_view_resize_timer = 0.0f;

    while (!window.should_close()) {
        frame_limiter.begin_frame();
        window.update_frame_stats();

        if (auto_close_seconds > 0.0f) {
            auto_close_timer += window.delta_time();
            if (auto_close_timer >= static_cast<double>(auto_close_seconds)) {
                GLOG_INFO("Auto-close after {} seconds", auto_close_seconds);
                window.request_close();
                break;
            }
        }

        // Play Mode CI 测试：第 1 秒进入，第 3 秒退出
        if (test_play_mode) {
            play_mode_test_timer += window.delta_time();
            if (!play_mode_test_entered && play_mode_test_timer >= 1.0) {
                play_mode_test_entered = true;
                enter_play_mode();
            }
            if (play_mode_test_entered && play_mode_test_timer >= 3.0 && play_mode_active) {
                exit_play_mode();
            }
        }

        // Delete + Undo CI 测试：第 1 秒删除 Ground，第 2.5 秒撤销
        if (test_delete_undo && world.scene()) {
            delete_undo_test_timer += window.delta_time();
            if (!delete_test_done && delete_undo_test_timer >= 1.0) {
                delete_test_done = true;
                if (scene::Entity* ground = world.scene()->find_entity_by_name("Ground")) {
                    hierarchy_panel->queue_delete(ground->uuid());
                    GLOG_INFO("[CI] Queued delete for entity 'Ground'");
                } else {
                    GLOG_WARN("[CI] Entity 'Ground' not found for delete test");
                }
            }
            if (delete_test_done && !undo_test_done && delete_undo_test_timer >= 2.5) {
                undo_test_done = true;
                undo_stack.undo();
                GLOG_INFO("[CI] Undo executed after delete");
            }
        }

        // 场景热重载计时器（实际重载在 present 之后执行）
        scene_reload_timer += window.delta_time();

        // 场景自动保存：间隔为 0 表示关闭；间隔被修改时重置计时器
        if (editor_settings.editor.autosave_interval_min != last_autosave_interval_min) {
            last_autosave_interval_min = editor_settings.editor.autosave_interval_min;
            autosave_timer = 0.0;
        }
        if (editor_settings.editor.autosave_interval_min > 0 && !play_mode_active) {
            autosave_timer += window.delta_time();
            if (autosave_timer >= editor_settings.editor.autosave_interval_min * 60.0) {
                autosave_timer = 0.0;
                if (world.scene() && world.scene()->has_unsaved_changes()) {
                    save_scene(scene_path);
                    GLOG_INFO("Scene autosaved to '{}'", scene_path);
                }
            }
        }

        input.update(&window);

        // F1 切换线框模式（调试用，仅 OpenGL）
        if (!is_vulkan && input.is_key_pressed(GLFW_KEY_F1)) {
            wireframe_mode = !wireframe_mode;
            render_ctx.push_command([wireframe_mode](render::IRenderBackend*) {
                glPolygonMode(GL_FRONT_AND_BACK, wireframe_mode ? GL_LINE : GL_FILL);
            });
            GLOG_INFO("Wireframe mode: {}", wireframe_mode ? "ON" : "OFF");
        }

        float dt = static_cast<float>(window.delta_time());
        if (dt > 0.1f) dt = 0.1f; // 防卡顿

        // 编辑器相机：只在 Viewport 面板悬停且 gizmo 未激活时响应输入
        editor_camera.update(dt, viewport_panel->hovered() && !viewport_panel->gizmo_active());

        // 驱动 ECS 系统（物理、动画、游戏逻辑等）
        world.update(dt);

        // -------------------------------------------------------------------
        // 窗口整体尺寸变化时强制同步渲染目标：解决全屏/最大化后画面消失
        // -------------------------------------------------------------------
        if (window_resize_state.pending.exchange(false)) {
            const int ww = window_resize_state.width.load();
            const int wh = window_resize_state.height.load();
            if (ww >= 4 && wh >= 4) {
                render_ctx.pause_render_thread();
                if (pipeline.resize_render_targets(ww, wh)) {
                    applied_vw = ww;
                    applied_vh = wh;
                    pending_vw = ww;
                    pending_vh = wh;
                }
                if (game_pipeline.is_valid() && game_pipeline.resize_render_targets(ww, wh)) {
                    applied_gw = ww;
                    applied_gh = wh;
                    pending_gw = ww;
                    pending_gh = wh;
                }
                render_ctx.resume_render_thread();
                viewport_resize_timer = k_resize_debounce;
                game_view_resize_timer = k_resize_debounce;
            }
        }

        // -------------------------------------------------------------------
        // Viewport 尺寸同步（上一帧面板尺寸，防抖后应用）
        // -------------------------------------------------------------------
        {
            const int cur_vw = static_cast<int>(viewport_panel->content_width());
            const int cur_vh = static_cast<int>(viewport_panel->content_height());
            if (cur_vw != pending_vw || cur_vh != pending_vh) {
                pending_vw = cur_vw;
                pending_vh = cur_vh;
                viewport_resize_timer = 0.0f;
            }
            viewport_resize_timer += dt;
            if (pending_vw >= 4 && pending_vh >= 4 &&
                (pending_vw != applied_vw || pending_vh != applied_vh) &&
                viewport_resize_timer >= k_resize_debounce) {
                render_ctx.pause_render_thread();
                if (pipeline.resize_render_targets(pending_vw, pending_vh)) {
                    applied_vw = pending_vw;
                    applied_vh = pending_vh;
                }
                render_ctx.resume_render_thread();
            }
        }

        // -------------------------------------------------------------------
        // Game View 尺寸同步（上一帧面板尺寸，防抖后应用）
        // 只在 Game View 为当前活动标签页时渲染，避免后台标签页浪费 GPU。
        // -------------------------------------------------------------------
        if (game_view_panel->is_active()) {
            const int cur_gw = static_cast<int>(game_view_panel->content_width());
            const int cur_gh = static_cast<int>(game_view_panel->content_height());
            if (cur_gw != pending_gw || cur_gh != pending_gh) {
                pending_gw = cur_gw;
                pending_gh = cur_gh;
                game_view_resize_timer = 0.0f;
            }
            game_view_resize_timer += dt;
            if (pending_gw >= 4 && pending_gh >= 4 &&
                (pending_gw != applied_gw || pending_gh != applied_gh) &&
                game_view_resize_timer >= k_resize_debounce) {
                render_ctx.pause_render_thread();
                if (game_pipeline.resize_render_targets(pending_gw, pending_gh)) {
                    applied_gw = pending_gw;
                    applied_gh = pending_gh;
                }
                render_ctx.resume_render_thread();
            }
        }

        int w = 0, h = 0;
        window.get_size(w, h);

        // 场景渲染分辨率：跟随 Viewport 面板尺寸
        const int render_w = (applied_vw >= 4) ? applied_vw : w;
        const int render_h = (applied_vh >= 4) ? applied_vh : h;
        const float aspect = (render_w > 0 && render_h > 0)
                                 ? static_cast<float>(render_w) / static_cast<float>(render_h)
                                 : 1.0f;
        camera.set_aspect(aspect);

        // Game View 渲染分辨率与宽高比
        const int game_render_w = (!is_vulkan && applied_gw >= 4) ? applied_gw : w;
        const int game_render_h = (!is_vulkan && applied_gh >= 4) ? applied_gh : h;
        const float game_aspect = (game_render_w > 0 && game_render_h > 0)
                                      ? static_cast<float>(game_render_w) / static_cast<float>(game_render_h)
                                      : 1.0f;
        game_camera.set_aspect(game_aspect);

        render_ctx.set_viewport(0, 0, w, h);

        // 将 Camera 组件参数（fov/near/far）同步到编辑器相机。
        // Play Mode 下不要把编辑器相机位置写回 MainCamera，否则游戏逻辑控制的相机会被覆盖，
        // Game View 也就无法显示真正的运行时视角。
        if (world.scene()) {
            apply_camera_component_to_global(*world.scene(), camera);
            if (!play_mode_active) {
                sync_active_camera_to_scene(*world.scene(), camera);
            }
        }

        // 设置渲染管线相机、灯光与视口
        pipeline.set_camera(camera);
        pipeline.set_lights(world.scene() ? collect_lights(*world.scene())
                                          : std::vector<render::RenderPipeline::Light>{});
        pipeline.set_viewport(render_w, render_h);

        // 从场景主摄像机构建 Game View 相机
        if (world.scene()) {
            build_game_camera(*world.scene(), game_camera);
        }
        game_pipeline.set_camera(game_camera);
        game_pipeline.set_lights(world.scene() ? collect_lights(*world.scene())
                                               : std::vector<render::RenderPipeline::Light>{});
        game_pipeline.set_viewport(game_render_w, game_render_h);

        // -------------------------------------------------------------------
        // 3D + 2D 场景渲染由 ECS World 驱动
        // -------------------------------------------------------------------
        if (fps_label) {
            fps_label->text = std::format("FPS: {:.1f}", window.fps());
        }

        if (renderer2d) {
            renderer2d->begin_frame(static_cast<float>(render_w), static_cast<float>(render_h));
        }

        world.render(render_ctx);

        if (renderer2d) {
            renderer2d->end_frame();
        }

        // -------------------------------------------------------------------
        // 编辑器 ImGui UI：DockSpace + 面板 + File 菜单弹窗 + 点选拾取
        // -------------------------------------------------------------------
        hierarchy_panel->set_scene(world.scene());
        inspector_panel->set_scene(world.scene());
        viewport_panel->set_scene(world.scene());
        inspector_panel->set_target(hierarchy_panel->selected_entity());

        imgui.begin_frame();
        ImGuizmo::BeginFrame();

#ifdef _WIN32
        // 绘制 FluentWindow 标题栏（用 BackgroundDrawList，不创建 ImGui 窗口）
        editor::FluentWindow_RenderTitleBar(window.native_handle());
#endif

        // 全局快捷键（跳过文本输入框获焦时）
        shortcuts.process();

        panel_manager.show();

        // Game View 独立渲染：在面板可见性/尺寸确定后再执行，避免为后台标签页浪费 GPU。
        // 渲染到自己的 FBO，随后显式切回默认 framebuffer 供 ImGui 绘制。
        if (game_view_panel->is_active() && game_pipeline.is_valid() && world.scene() &&
            game_render_w >= 4 && game_render_h >= 4) {
            game_pipeline.render_scene(*world.scene(), render_ctx);
        }

        // 3D/2D 场景（含 Game View）已写入各自离屏 FBO，ImGui 必须回到默认 framebuffer 绘制，
        // 否则 swap buffers 时屏幕显示的是未初始化的默认 framebuffer（全黑）。
        render_ctx.set_framebuffer(render::RHIFramebufferHandle{});
        render_ctx.clear(0.1f, 0.1f, 0.1f, 1.0f);

        // 设置窗口（File > Settings / File > Project Settings）
        settings_window.draw(project_root.string(), editor_settings);
        project_settings_window.draw(project_root.string(), project_settings);
        gimport_editor_window.draw();

        // 通用警告/错误弹窗（各面板通过 MessagePopup::warn/error 登记）
        editor::MessagePopup::instance().draw();

        // Save Scene As 弹窗
        if (save_as_popup_requested) {
            ImGui::OpenPopup("SaveSceneAs");
            save_as_popup_requested = false;
        }
        if (ImGui::BeginPopupModal((std::string(editor::tr("dialog.save_scene_as")) + "###SaveSceneAs").c_str(),
                                   nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("%s", editor::tr("dialog.save_scene_to"));
            ImGui::InputText("##save_as_path", save_as_buf, sizeof(save_as_buf));
            if (ImGui::Button(editor::tr("dialog.save")) ||
                (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Enter, false))) {
                save_scene(save_as_buf);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(editor::tr("dialog.cancel"))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Open Scene 弹窗
        if (open_popup_requested) {
            ImGui::OpenPopup("OpenScene");
            open_popup_requested = false;
        }
        if (ImGui::BeginPopupModal((std::string(editor::tr("dialog.open_scene")) + "###OpenScene").c_str(),
                                   nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("%s", editor::tr("dialog.open_scene_from"));
            ImGui::InputText("##open_path", open_buf, sizeof(open_buf));
            if (ImGui::Button(editor::tr("dialog.open")) ||
                (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Enter, false))) {
                open_scene(open_buf);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(editor::tr("dialog.cancel"))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Load Project 弹窗
        if (load_project_popup_requested) {
            ImGui::OpenPopup("LoadProject");
            load_project_popup_requested = false;
        }
        if (ImGui::BeginPopupModal((std::string(editor::tr("dialog.load_project")) + "###LoadProject").c_str(),
                                   nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("%s", editor::tr("dialog.load_project_from"));
            ImGui::InputText("##load_project_path", load_project_buf, sizeof(load_project_buf));
            if (ImGui::Button(editor::tr("dialog.load")) ||
                (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Enter, false))) {
                load_project(load_project_buf);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(editor::tr("dialog.cancel"))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // 点选拾取：Viewport 左击 UV → NDC → 世界射线 → AABB 求交（非 Play 状态才生效）
        ImVec2 pick_uv;
        if (!play_mode_active && world.scene() && viewport_panel->take_pick_click(pick_uv)) {
            const float ndc_x = pick_uv.x * 2.0f - 1.0f;
            const float ndc_y = 1.0f - pick_uv.y * 2.0f; // ImGui y 向下 → NDC y 向上
            const math::Matrix4f inv_vp =
                (camera.get_projection_matrix() * camera.get_view_matrix()).inverse();
            const math::Ray ray = math::screen_ndc_to_ray(ndc_x, ndc_y, inv_vp);
            if (scene::Entity* hit = pick_entity(*world.scene(), ray)) {
                hierarchy_panel->select(hit->uuid());
                GLOG_INFO("Picked entity '{}'", hit->name());
            } else {
                hierarchy_panel->clear_selection();
            }
        }

        imgui.end_frame([&](ImDrawData* draw_data, std::shared_ptr<std::promise<void>> sync_promise) {
            // 深拷贝 ImDrawData，避免无限制帧率下主线程继续 NewFrame 覆盖 draw data
            auto owned_draw_data = imgui.clone_draw_data(draw_data);
            render_ctx.push_command([owned_draw_data, &imgui, sync_promise](render::IRenderBackend*) {
                imgui.render_draw_data(owned_draw_data.get());
                sync_promise->set_value();
            });
        });

        render_ctx.present();

        // 录制：捕获当前帧 RGBA 并写入 PNG 序列
        if (recorder && recorder->frame_count() < static_cast<int>(args.record_seconds * 30.0f + 0.5f)) {
            std::vector<uint8_t> rgba;
            int cw = 0, ch = 0;
            if (render_ctx.capture_frame_rgba(rgba, cw, ch)) {
                recorder->write_frame(rgba.data(), cw, ch);
            }
        }

        // 在 present 之后执行 hierarchy 的删除/换父等延迟操作，确保渲染线程已完成
        // 本帧所有引用这些实体的绘制命令，避免删除节点时访问已释放资源而崩溃。
        hierarchy_panel->flush_deferred_ops();

        // -------------------------------------------------------------------
        // 场景热重载：必须在 present 之后执行，
        // 这样 command buffer 为空，pause 不会丢失未提交的渲染命令。
        // -------------------------------------------------------------------
        if (!is_vulkan && scene_reload_timer >= 1.0) {
            scene_reload_timer = 0.0;
            auto new_time = get_scene_write_time(scene_path);
            if (new_time != std::filesystem::file_time_type::min() &&
                new_time != scene_last_write) {
                // 热重载前保存当前选中实体的 UUID，重载后按 UUID 恢复选中
                const scene::UUID pre_reload_selection =
                    hierarchy_panel->selected_entity()
                        ? hierarchy_panel->selected_entity()->uuid()
                        : scene::UUID::nil();

                auto reloaded = try_reload_scene(scene_path, world.detach_scene(),
                                                 new_time, scene_last_write);

                if (reloaded) {
                    render_ctx.pause_render_thread();

                    // 确保物理演示场景有地面和可下落的 cube
                    ensure_physics_demo_entities(*reloaded);

                    upload_scene_meshes(*reloaded, render_ctx);
                    ensure_scene_defaults(*reloaded, camera);
                    sync_editor_to_scene_camera(*reloaded, camera);
                    world.attach_scene(std::move(reloaded));
                    render_ctx.resume_render_thread();

                    // 恢复选中：新场景中若仍存在同 UUID 实体则保留选中，否则清空
                    if (pre_reload_selection.is_valid()) {
                        hierarchy_panel->select(pre_reload_selection);
                    } else {
                        hierarchy_panel->clear_selection();
                    }

                    fps_label = nullptr;
                    scene::Entity* fps_entity = world.scene()->find_entity_by_name("FPS_Label");
                    if (fps_entity) {
                        fps_label = fps_entity->get_component<components::d2::text::Label>();
                    }
                } else {
                    hierarchy_panel->clear_selection();
                }
            }
        }

        window.poll_events();

        // 用户点了关闭按钮后立即退出，不再跑下一帧
        if (window.should_close() || window.close_requested()) {
            GLOG_INFO("Editor loop: close requested, breaking");
            frame_limiter.end_frame();
            break;
        }

        // CPU 侧帧率限制放在 poll_events 之后，这样 glfwPollEvents 的开销也被
        // 算进目标帧时间，实际 FPS 才会接近设定值。
        frame_limiter.end_frame();
    }

    GLOG_INFO("Editor loop exited. FPS: {:.1f}", window.fps());

    // 完成视频编码
    if (recorder) {
        recorder->finalize();
    }

    // 退出时保存场景
    if (world.scene()) {
        scene::SceneSerializer::save_to_file(*world.scene(), scene_path);
        GLOG_INFO("Scene saved to '{}'", scene_path);
    }

    // 退出顺序：
    // 1) 先暂停渲染线程并等待 GPU 空闲，防止后续资源销毁时命令仍在执行；
    // 2) 停止 ECS System；
    // 3) 析构 Scene Entity/Component；
    // 4) 销毁 2D / ImGui / Pipeline；
    // 5) 关闭 RenderContext。
    render_ctx.pause_render_thread_keep_cmdbuffer();
    world.shutdown();
    {
        auto scene = world.detach_scene();
        scene.reset();
    }

    if (renderer2d) {
        renderer2d->shutdown();
    }
    imgui.shutdown();
    pipeline.shutdown();
    render_ctx.shutdown();

    // 先显式销毁窗口，再 glfwTerminate；顺序反过来可能导致延迟或异常。
    window.destroy();
    platform::Window::shutdown_sdk();
    utils::glog_shutdown();

    std::cout << "All systems nominal." << std::endl;
    return 0;
}

} // namespace gryce_engine::editor
