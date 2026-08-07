#include "GrycePhysics/physics_api.h"
#include "GryceCore/core_api.h"

#include "physics/physics_factory.h"
#include "physics/physics_world_3d.h"
#include "physics/physics_world_2d.h"
#include "physics/physics_types.h"
#include "ecs/world.h"
#include "ecs/systems/physics_system_2d.h"
#include "ecs/systems/physics_system_3d.h"
#include "utils/glog/glog_lib.h"

#include <memory>
#include <mutex>
#include <unordered_map>
#include <cstring>

using gryce_engine::physics::IPhysicsWorld3D;
using gryce_engine::physics::IPhysicsWorld2D;
using gryce_engine::physics::create_physics_world_3d;
using gryce_engine::physics::create_physics_world_2d;
using gryce_engine::physics::BodyDesc;
using gryce_engine::physics::BodyType;
using gryce_engine::physics::k_invalid_body;
using gryce_engine::physics::RaycastHit;
using gryce_engine::math::Vector3f;
using gryce_engine::math::Quaternionf;

namespace {

struct PhysicsState {
    bool initialized = false;
    GPhysicsBackend backend = GPHYSICS_BACKEND_JOLT;
    std::unique_ptr<IPhysicsWorld3D> world_3d;
    std::unique_ptr<IPhysicsWorld2D> world_2d;
    std::mutex mutex;

    // Body tracking: GBodyHandle (int) -> internal BodyHandle (uint32_t)
    std::unordered_map<int, gryce_engine::physics::BodyHandle> body_map;
    int next_body_handle = 1;
};

static PhysicsState g_physics;

} // namespace

extern "C" {

int GPhysics_Init(GPhysicsBackend backend) {
    std::lock_guard lock(g_physics.mutex);
    if (g_physics.initialized) return 0;

    g_physics.backend = backend;

    const char* name = (backend == GPHYSICS_BACKEND_BOX2D) ? "box2d" : "jolt";

    // Create 3D world
    g_physics.world_3d = create_physics_world_3d(name);
    if (g_physics.world_3d) {
        if (!g_physics.world_3d->init()) {
            GLOG_WARN("GPhysics_Init: 3D world init failed");
            g_physics.world_3d.reset();
        }
    }

    // Create 2D world
    g_physics.world_2d = create_physics_world_2d(name);
    if (g_physics.world_2d) {
        if (!g_physics.world_2d->init()) {
            GLOG_WARN("GPhysics_Init: 2D world init failed");
            g_physics.world_2d.reset();
        }
    }

    g_physics.initialized = true;
    GLOG_INFO("GPhysics_Init: backend={}", name);
    return 0;
}

void GPhysics_Shutdown(void) {
    std::lock_guard lock(g_physics.mutex);
    if (!g_physics.initialized) return;

    if (g_physics.world_3d) {
        g_physics.world_3d->shutdown();
        g_physics.world_3d.reset();
    }
    if (g_physics.world_2d) {
        g_physics.world_2d->shutdown();
        g_physics.world_2d.reset();
    }

    g_physics.body_map.clear();
    g_physics.next_body_handle = 1;
    g_physics.initialized = false;
}

int GPhysics_AttachSystems(void* world_ptr) {
    auto* world = static_cast<gryce_engine::ecs::World*>(world_ptr);
    if (!world) {
        GLOG_ERROR("GPhysics_AttachSystems: null world pointer");
        return -1;
    }
    world->register_system(std::make_unique<gryce_engine::ecs::PhysicsSystem3D>());
    world->register_system(std::make_unique<gryce_engine::ecs::PhysicsSystem2D>());
    GLOG_INFO("GPhysics_AttachSystems: PhysicsSystem3D + PhysicsSystem2D attached");
    return 0;
}

void GPhysics_SetGravity(const GVec3* gravity) {
    if (!gravity) return;
    std::lock_guard lock(g_physics.mutex);
    if (g_physics.world_3d) {
        g_physics.world_3d->set_gravity(Vector3f(gravity->x, gravity->y, gravity->z));
    }
}

void GPhysics_Step(float dt, int substeps) {
    std::lock_guard lock(g_physics.mutex);
    if (g_physics.world_3d) {
        g_physics.world_3d->step(dt, substeps > 0 ? substeps : 1);
    }
    if (g_physics.world_2d) {
        g_physics.world_2d->step(dt, 8, 3);
    }
}

GBodyHandle GPhysics_CreateBody(GEntityHandle entity, bool is_static) {
    (void)entity;
    std::lock_guard lock(g_physics.mutex);
    if (!g_physics.world_3d) return 0;

    BodyDesc desc;
    desc.type = is_static ? BodyType::Static : BodyType::Dynamic;
    // Default position/rotation - will be synced from entity transform

    auto handle = g_physics.world_3d->create_body(desc);
    if (handle == k_invalid_body) return 0;

    int api_handle = g_physics.next_body_handle++;
    g_physics.body_map[api_handle] = handle;
    return api_handle;
}

void GPhysics_DestroyBody(GBodyHandle body) {
    if (body <= 0) return;
    std::lock_guard lock(g_physics.mutex);
    auto it = g_physics.body_map.find(body);
    if (it != g_physics.body_map.end()) {
        if (g_physics.world_3d) {
            g_physics.world_3d->destroy_body(it->second);
        }
        g_physics.body_map.erase(it);
    }
}

void GPhysics_SetBodyTransform(GBodyHandle body, const GVec3* pos, const GQuat* rot) {
    if (body <= 0 || !pos || !rot) return;
    std::lock_guard lock(g_physics.mutex);
    auto it = g_physics.body_map.find(body);
    if (it != g_physics.body_map.end() && g_physics.world_3d) {
        g_physics.world_3d->set_transform(it->second,
            Vector3f(pos->x, pos->y, pos->z),
            Quaternionf(rot->x, rot->y, rot->z, rot->w));
    }
}

void GPhysics_GetBodyTransform(GBodyHandle body, GVec3* out_pos, GQuat* out_rot) {
    if (body <= 0 || !out_pos || !out_rot) return;
    std::lock_guard lock(g_physics.mutex);
    auto it = g_physics.body_map.find(body);
    if (it != g_physics.body_map.end() && g_physics.world_3d) {
        Vector3f pos;
        Quaternionf rot;
        g_physics.world_3d->get_transform(it->second, pos, rot);
        out_pos->x = pos.x; out_pos->y = pos.y; out_pos->z = pos.z;
        out_rot->x = rot.x; out_rot->y = rot.y; out_rot->z = rot.z; out_rot->w = rot.w;
    }
}

void GPhysics_AddForce(GBodyHandle body, const GVec3* force) {
    if (body <= 0 || !force) return;
    std::lock_guard lock(g_physics.mutex);
    auto it = g_physics.body_map.find(body);
    if (it != g_physics.body_map.end() && g_physics.world_3d) {
        g_physics.world_3d->apply_force(it->second,
            Vector3f(force->x, force->y, force->z),
            Vector3f::zero());
    }
}

void GPhysics_AddImpulse(GBodyHandle body, const GVec3* impulse) {
    if (body <= 0 || !impulse) return;
    std::lock_guard lock(g_physics.mutex);
    auto it = g_physics.body_map.find(body);
    if (it != g_physics.body_map.end() && g_physics.world_3d) {
        g_physics.world_3d->apply_impulse(it->second,
            Vector3f(impulse->x, impulse->y, impulse->z),
            Vector3f::zero());
    }
}

bool GPhysics_Raycast(const GVec3* origin, const GVec3* dir, float max_dist,
                       GVec3* out_hit_point, GVec3* out_hit_normal,
                       GEntityHandle* out_entity) {
    if (!origin || !dir) return false;
    std::lock_guard lock(g_physics.mutex);
    if (!g_physics.world_3d) return false;

    auto hit = g_physics.world_3d->raycast(
        Vector3f(origin->x, origin->y, origin->z),
        Vector3f(dir->x, dir->y, dir->z),
        max_dist);

    if (!hit.has_value()) return false;

    if (out_hit_point) {
        out_hit_point->x = hit->point.x;
        out_hit_point->y = hit->point.y;
        out_hit_point->z = hit->point.z;
    }
    if (out_hit_normal) {
        out_hit_normal->x = hit->normal.x;
        out_hit_normal->y = hit->normal.y;
        out_hit_normal->z = hit->normal.z;
    }
    if (out_entity) {
        *out_entity = 0; // TODO: map body handle back to entity
    }
    return true;
}

} // extern "C"
