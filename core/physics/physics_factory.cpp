#include "physics/physics_factory.h"

#include "physics/box2d_world_2d.h"
#include "physics/builtin_physics_world_2d.h"
#include "physics/builtin_physics_world_3d.h"

#include "utils/glog/glog_lib.h"

namespace gryce_engine::physics {

std::unique_ptr<IPhysicsWorld2D> create_physics_world_2d(const std::string& backend_name) {
    if (backend_name == "box2d") {
#ifdef GRYCE_HAS_BOX2D
        return std::make_unique<Box2DPhysicsWorld2D>();
#else
        GLOG_WARN("create_physics_world_2d: Box2D not available, falling back to builtin");
        return std::make_unique<BuiltinPhysicsWorld2D>();
#endif
    }
    if (backend_name == "builtin" || backend_name.empty()) {
        return std::make_unique<BuiltinPhysicsWorld2D>();
    }
    GLOG_WARN("create_physics_world_2d: unknown backend '{}', falling back to builtin", backend_name);
    return std::make_unique<BuiltinPhysicsWorld2D>();
}

std::unique_ptr<IPhysicsWorld3D> create_physics_world_3d(const std::string& backend_name) {
    if (backend_name == "builtin" || backend_name.empty()) {
        return std::make_unique<BuiltinPhysicsWorld3D>();
    }
#ifdef GRYCE_HAS_JOLT
    if (backend_name == "jolt") {
        // TODO: return std::make_unique<JoltPhysicsWorld3D>();
    }
#endif
    GLOG_WARN("create_physics_world_3d: backend '{}' not available, falling back to builtin", backend_name);
    return std::make_unique<BuiltinPhysicsWorld3D>();
}

} // namespace gryce_engine::physics
