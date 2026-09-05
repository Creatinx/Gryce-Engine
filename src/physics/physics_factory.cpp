#include "physics/physics_factory.h"

#include "physics/box2d_world_2d.h"
#include "physics/jolt_physics_world_3d.h"

#include "utils/glog/glog_lib.h"

namespace gryce_engine::physics {

std::unique_ptr<IPhysicsWorld2D> create_physics_world_2d(const std::string& backend_name) {
    if (backend_name == "box2d" || backend_name.empty()) {
#ifdef GRYCE_HAS_BOX2D
        return std::make_unique<Box2DPhysicsWorld2D>();
#else
        GLOG_ERROR("create_physics_world_2d: Box2D backend requested but GRYCE_HAS_BOX2D is not defined. "
                   "Pass -DGRYCE_FETCH_BOX2D=ON or install Box2D and pass -DGRYCE_HAS_BOX2D=ON.");
        return nullptr;
#endif
    }
    GLOG_ERROR("create_physics_world_2d: unknown backend '{}'. Only 'box2d' is supported.", backend_name);
    return nullptr;
}

std::unique_ptr<IPhysicsWorld3D> create_physics_world_3d(const std::string& backend_name) {
    if (backend_name == "jolt" || backend_name.empty()) {
#ifdef GRYCE_HAS_JOLT
        return std::make_unique<JoltPhysicsWorld3D>();
#else
        GLOG_ERROR("create_physics_world_3d: Jolt backend requested but GRYCE_HAS_JOLT is not defined. "
                   "Pass -DGRYCE_FETCH_JOLT=ON or install Jolt and pass -DGRYCE_HAS_JOLT=ON.");
        return nullptr;
#endif
    }
    GLOG_ERROR("create_physics_world_3d: unknown backend '{}'. Only 'jolt' is supported.", backend_name);
    return nullptr;
}

} // namespace gryce_engine::physics
