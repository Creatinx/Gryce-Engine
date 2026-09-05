#include "GryceCore/scene_api.h"
#include "GryceCore/api_guard.h"
#include "runtime/engine_context.h"

#include "scene/scene_serializer.h"
#include "scene/scene.h"
#include "ecs/world.h"
#include "scene/query.h"
#include "math/ray.h"
#include "resources/resource_path.h"
#include "components/camera.h"
#include "components/mesh_renderer.h"
#include "components/skinned_mesh_renderer.h"
#include "assets/asset_manager.h"
#include "utils/glog/glog_lib.h"

#include <algorithm>
#include <filesystem>
#include <limits>
#include <vector>

using gryce_engine::scene::SceneSerializer;

namespace gc = gryce_core;

namespace {

// 实体世界矩阵（沿父链组合局部变换；根实体视为恒等）
gryce_engine::math::Matrix4f entity_world_matrix(gryce_engine::scene::Entity* e) {
    std::vector<gryce_engine::scene::Entity*> chain;
    for (gryce_engine::scene::Entity* cur = e; cur && cur->parent(); cur = cur->parent()) {
        chain.push_back(cur);
    }
    gryce_engine::math::Matrix4f m = gryce_engine::math::Matrix4f::identity();
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        if (auto* t = (*it)->transform()) m = m * t->local_matrix();
    }
    return m;
}

// 由实体 Transform + Camera 组件构造 (projection * view) 的逆矩阵
bool camera_inv_view_proj(gryce_engine::scene::Entity* cam_entity,
                          int vp_w, int vp_h,
                          gryce_engine::math::Matrix4f& out) {
    if (!cam_entity) return false;
    auto* cam = cam_entity->get_component<gryce_engine::components::Camera>();
    auto* t = cam_entity->transform();
    if (!cam || !cam->enabled || !t) return false;

    const gryce_engine::math::Vector3f fwd =
        t->rotation.rotate_vector(gryce_engine::math::Vector3f(0.0f, 0.0f, -1.0f)).normalized();
    const gryce_engine::math::Vector3f up =
        t->rotation.rotate_vector(gryce_engine::math::Vector3f(0.0f, 1.0f, 0.0f)).normalized();
    const float aspect = vp_h > 0 ? static_cast<float>(vp_w) / static_cast<float>(vp_h) : 16.0f / 9.0f;
    const gryce_engine::math::Matrix4f view =
        gryce_engine::math::Matrix4f::look_at(t->position, t->position + fwd, up);
    const gryce_engine::math::Matrix4f proj =
        gryce_engine::math::Matrix4f::perspective(cam->fov, aspect, cam->near_plane, cam->far_plane);
    out = (proj * view).inverse();
    return true;
}

// 网格局部顶点 → 世界 AABB（八顶点变换近似：直接用变换后的顶点极值）
bool world_aabb_of_vertices(const std::vector<gryce_engine::assets::MeshVertex>& vertices,
                            const gryce_engine::math::Matrix4f& world,
                            gryce_engine::math::Vector3f& bmin,
                            gryce_engine::math::Vector3f& bmax) {
    if (vertices.empty()) return false;
    bool first = true;
    for (const auto& v : vertices) {
        const gryce_engine::math::Vector4f p =
            world * gryce_engine::math::Vector4f(v.position.x, v.position.y, v.position.z, 1.0f);
        const gryce_engine::math::Vector3f wp(p.x, p.y, p.z);
        if (first) {
            bmin = bmax = wp;
            first = false;
        } else {
            bmin.x = std::min(bmin.x, wp.x); bmin.y = std::min(bmin.y, wp.y); bmin.z = std::min(bmin.z, wp.z);
            bmax.x = std::max(bmax.x, wp.x); bmax.y = std::max(bmax.y, wp.y); bmax.z = std::max(bmax.z, wp.z);
        }
    }
    return !first;
}

// 射线拾取核心：遍历网格实体求世界 AABB 最近命中
GEntityHandle pick_with_ray(gryce_engine::scene::Scene& scn,
                            const gryce_engine::math::Ray& ray,
                            float max_dist) {
    float best_t = max_dist > 0.0f ? max_dist : std::numeric_limits<float>::max();
    GEntityHandle best = 0;

    scn.foreach([&](gryce_engine::scene::Entity* e) {
        if (!e || e->parent() == nullptr || !e->enabled) return;

        const gryce_engine::math::Matrix4f world = entity_world_matrix(e);
        gryce_engine::math::Vector3f bmin, bmax;
        bool has_bounds = false;

        if (auto* mr = e->get_component<gryce_engine::components::MeshRenderer>()) {
            if (mr->enabled && !mr->mesh_path.empty()) {
                auto mesh = gryce_engine::assets::AssetManager::instance().load_mesh(mr->mesh_path);
                if (mesh) has_bounds = world_aabb_of_vertices(mesh->vertices, world, bmin, bmax);
            }
        }
        if (!has_bounds) {
            if (auto* smr = e->get_component<gryce_engine::components::SkinnedMeshRenderer>()) {
                if (smr->enabled && !smr->model_path.empty()) {
                    auto model = gryce_engine::assets::AssetManager::instance().load_skinned_model(smr->model_path);
                    if (model) {
                        for (const auto& m : model->meshes) {
                            gryce_engine::math::Vector3f mi, ma;
                            if (world_aabb_of_vertices(m.vertices, world, mi, ma)) {
                                if (!has_bounds) { bmin = mi; bmax = ma; has_bounds = true; }
                                else {
                                    bmin.x = std::min(bmin.x, mi.x); bmin.y = std::min(bmin.y, mi.y); bmin.z = std::min(bmin.z, mi.z);
                                    bmax.x = std::max(bmax.x, ma.x); bmax.y = std::max(bmax.y, ma.y); bmax.z = std::max(bmax.z, ma.z);
                                }
                            }
                        }
                    }
                }
            }
        }
        if (!has_bounds) return;

        float t = 0.0f;
        if (gryce_engine::math::ray_intersect_aabb(ray, bmin, bmax, t) && t < best_t) {
            best_t = t;
            best = gc::g_core_state.entity_map.lookup(e->uuid());
        }
    });
    return best;
}

} // namespace

extern "C" {

int GScene_Load(const char* path) {
    GRYCE_API_GUARD();
    if (!gc::g_core_state.initialized || !path || !path[0]) return -1;

    auto scene = SceneSerializer::load_from_file(path);
    if (!scene) return -1;

    if (gc::g_core_state.world) {
        gc::g_core_state.world->attach_scene(std::move(scene));
    }
    gc::g_core_state.current_scene_path = path;
    if (gc::g_core_state.scene_mode == 0) gc::g_core_state.scene_path_2d = path;
    else gc::g_core_state.scene_path_3d = path;
    gc::g_core_state.entity_map.rebuild(gc::g_core_state.world->scene());
    gc::g_core_state.selected_entity = 0;
    gc::g_core_state.deferred_entity_list_changed = true;
    gc::g_core_state.deferred_scene_loaded = true;
    return 0;
}

int GScene_Save(const char* path) {
    GRYCE_API_GUARD();
    if (!gc::g_core_state.initialized || !gc::g_core_state.world || !gc::g_core_state.world->scene() || !path) return -1;
    const bool ok = SceneSerializer::save_to_file(*gc::g_core_state.world->scene(), path);
    if (ok) {
        gc::g_core_state.current_scene_path = path;
        if (gc::g_core_state.scene_mode == 0) gc::g_core_state.scene_path_2d = path;
        else gc::g_core_state.scene_path_3d = path;
    }
    return ok ? 0 : -1;
}

int GScene_GetCurrentPath(char* out_buf, int buf_size) {
    GRYCE_API_GUARD();
    if (!out_buf || buf_size <= 0) return -1;
    std::strncpy(out_buf, gc::g_core_state.current_scene_path.c_str(), static_cast<size_t>(buf_size) - 1);
    out_buf[buf_size - 1] = '\0';
    return static_cast<int>(std::strlen(out_buf));
}

int GScene_New(void) {
    GRYCE_API_GUARD();
    if (!gc::g_core_state.initialized) return -1;
    const std::string default_path =
        gc::g_core_state.scene_mode == 0 ? "res:/scenes/scene_2d.gesc" : "res:/scenes/scene_3d.gesc";
    auto scene = std::make_unique<gryce_engine::scene::Scene>(
        gc::g_core_state.scene_mode == 0 ? "scene_2d" : "scene_3d");
    if (gc::g_core_state.world) {
        gc::g_core_state.world->attach_scene(std::move(scene));
        // 新建场景写入当前模式的缓冲场景文件（2D/3D 各自独立，绝不合并）
        gryce_engine::scene::SceneSerializer::save_to_file(
            *gc::g_core_state.world->scene(), default_path);
    }
    gc::g_core_state.current_scene_path = default_path;
    if (gc::g_core_state.scene_mode == 0) gc::g_core_state.scene_path_2d = default_path;
    else gc::g_core_state.scene_path_3d = default_path;
    gc::g_core_state.entity_map.rebuild(gc::g_core_state.world->scene());
    gc::g_core_state.selected_entity = 0;
    gc::g_core_state.deferred_entity_list_changed = true;
    return 0;
}

GEntityHandle GScene_PickScreen(float sx, float sy, int viewport_w, int viewport_h,
                                GEntityHandle camera_entity) {
    GRYCE_API_GUARD();
    if (!gc::g_core_state.initialized || !gc::g_core_state.world || !gc::g_core_state.world->scene()) return 0;
    if (viewport_w <= 0 || viewport_h <= 0) return 0;

    gryce_engine::scene::Entity* cam = gc::EntityResolver::resolve(camera_entity);
    if (!cam) {
        // 未指定相机：使用场景第一个启用的 Camera 实体
        gryce_engine::scene::Scene* s = gc::g_core_state.world->scene();
        s->foreach([&](gryce_engine::scene::Entity* e) {
            if (!cam && e) {
                auto* c = e->get_component<gryce_engine::components::Camera>();
                if (c && c->enabled) cam = e;
            }
        });
    }

    gryce_engine::math::Matrix4f inv_vp;
    if (!camera_inv_view_proj(cam, viewport_w, viewport_h, inv_vp)) return 0;

    // 屏幕坐标：左上原点、Y 向下 → NDC：y 向上
    const float ndc_x = (sx / static_cast<float>(viewport_w)) * 2.0f - 1.0f;
    const float ndc_y = 1.0f - (sy / static_cast<float>(viewport_h)) * 2.0f;
    const gryce_engine::math::Ray ray = gryce_engine::math::screen_ndc_to_ray(ndc_x, ndc_y, inv_vp);
    return pick_with_ray(*gc::g_core_state.world->scene(), ray, 0.0f);
}

GEntityHandle GScene_PickRay(const GVec3* origin, const GVec3* direction, float max_dist) {
    GRYCE_API_GUARD();
    if (!gc::g_core_state.initialized || !gc::g_core_state.world || !gc::g_core_state.world->scene()) return 0;
    if (!origin || !direction) return 0;

    gryce_engine::math::Ray ray;
    ray.origin = gryce_engine::math::Vector3f(origin->x, origin->y, origin->z);
    gryce_engine::math::Vector3f dir(direction->x, direction->y, direction->z);
    const float len = dir.length();
    if (len < 1e-8f) return 0;
    ray.direction = dir / len;
    return pick_with_ray(*gc::g_core_state.world->scene(), ray, max_dist);
}

int GScene_GetMode(void) {
    GRYCE_API_GUARD();
    return gc::g_core_state.scene_mode;
}

int GScene_SetMode(int mode) {
    GRYCE_API_GUARD();
    if (mode != 0 && mode != 1) return -1;
    if (!gc::g_core_state.initialized || !gc::g_core_state.world) return -1;
    if (mode == gc::g_core_state.scene_mode) return 0;

    // 保存当前场景到旧槽（detach 保留场景内存，仅停止渲染/物理引用）
    if (gc::g_core_state.world->scene()) {
        auto detached = gc::g_core_state.world->detach_scene();
        if (gc::g_core_state.scene_mode == 0) gc::g_core_state.scene_slot_2d = std::move(detached);
        else gc::g_core_state.scene_slot_3d = std::move(detached);
    }
    if (gc::g_core_state.scene_mode == 0) {
        gc::g_core_state.scene_path_2d = gc::g_core_state.current_scene_path;
    } else {
        gc::g_core_state.scene_path_3d = gc::g_core_state.current_scene_path;
    }

    gc::g_core_state.scene_mode = mode;
    auto& slot = mode == 0 ? gc::g_core_state.scene_slot_2d : gc::g_core_state.scene_slot_3d;
    if (slot) {
        gc::g_core_state.world->attach_scene(std::move(slot));
    } else {
        // 槽没有场景：使用该模式专属的缓冲场景文件（2D/3D 各自独立，绝不合并）。
        // 文件已存在则加载（跨会话保留），否则新建空场景并写入缓冲文件。
        const std::string default_path =
            mode == 0 ? "res:/scenes/scene_2d.gesc" : "res:/scenes/scene_3d.gesc";
        const std::string resolved = gryce_engine::resources::ResourcePath::resolve(default_path);

        std::unique_ptr<gryce_engine::scene::Scene> next;
        if (std::filesystem::exists(resolved)) {
            next = gryce_engine::scene::SceneSerializer::load_from_file(default_path);
        }
        if (!next) {
            next = std::make_unique<gryce_engine::scene::Scene>(
                mode == 0 ? "scene_2d" : "scene_3d");
            // 新建缓冲场景文件（项目 scenes/ 目录）
            gryce_engine::scene::SceneSerializer::save_to_file(*next, default_path);
        }
        if (mode == 0) gc::g_core_state.scene_path_2d = default_path;
        else gc::g_core_state.scene_path_3d = default_path;
        gc::g_core_state.world->attach_scene(std::move(next));
    }
    gc::g_core_state.current_scene_path =
        mode == 0 ? gc::g_core_state.scene_path_2d : gc::g_core_state.scene_path_3d;
    gc::g_core_state.entity_map.rebuild(gc::g_core_state.world->scene());
    gc::g_core_state.selected_entity = 0;
    gc::g_core_state.deferred_entity_list_changed = true;
    gc::g_core_state.deferred_scene_loaded = true;
    return 0;
}

int GScene_ReleaseMode(int mode) {
    GRYCE_API_GUARD();
    if (mode != 0 && mode != 1) return -1;
    if (!gc::g_core_state.initialized) return -1;

    if (mode == gc::g_core_state.scene_mode) {
        // 释放活动场景：替换为空场景
        const std::string default_path =
            mode == 0 ? "res:/scenes/scene_2d.gesc" : "res:/scenes/scene_3d.gesc";
        auto next = std::make_unique<gryce_engine::scene::Scene>(
            mode == 0 ? "scene_2d" : "scene_3d");
        gryce_engine::scene::SceneSerializer::save_to_file(*next, default_path);
        if (gc::g_core_state.world) {
            gc::g_core_state.world->attach_scene(std::move(next));
            gc::g_core_state.entity_map.rebuild(gc::g_core_state.world->scene());
            gc::g_core_state.selected_entity = 0;
            gc::g_core_state.deferred_entity_list_changed = true;
        }
        gc::g_core_state.current_scene_path = default_path;
    } else {
        if (mode == 0) gc::g_core_state.scene_slot_2d.reset();
        else gc::g_core_state.scene_slot_3d.reset();
    }
    if (mode == 0) gc::g_core_state.scene_path_2d = "res:/scenes/scene_2d.gesc";
    else gc::g_core_state.scene_path_3d = "res:/scenes/scene_3d.gesc";
    return 0;
}

bool GScene_HasScene(int mode) {
    GRYCE_API_GUARD();
    if (mode == gc::g_core_state.scene_mode) return true;
    return mode == 0 ? (bool)gc::g_core_state.scene_slot_2d : (bool)gc::g_core_state.scene_slot_3d;
}

} // extern "C"
