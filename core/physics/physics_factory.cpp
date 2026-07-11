#include "physics/physics_factory.h"

#include "physics/box2d_world_2d.h"
#include "physics/builtin_physics_world_2d.h"
#include "physics/builtin_physics_world_3d.h"
#include "physics/jolt_physics_world_3d.h"

#include "utils/glog/glog_lib.h"

namespace gryce_engine::physics {

std::unique_ptr<IPhysicsWorld2D> create_physics_world_2d(const std::string& backend_name) {
    // 默认优先 Box2D；只有显式传入 "builtin" 才使用自研实现
    if (backend_name == "box2d" || backend_name.empty()) {
#ifdef GRYCE_HAS_BOX2D
        return std::make_unique<Box2DPhysicsWorld2D>();
#else
        if (backend_name == "box2d") {
            GLOG_WARN("create_physics_world_2d: Box2D requested but not available, falling back to builtin");
        }
        GLOG_WARN("create_physics_world_2d: using builtin physics (unstable, may cause errors). "
                  "Consider building with GRYCE_HAS_BOX2D=ON for stable 2D physics.");
        return std::make_unique<BuiltinPhysicsWorld2D>();
#endif
    }
    if (backend_name == "builtin") {
        GLOG_WARN("create_physics_world_2d: using builtin physics (unstable, may cause errors). "
                  "Consider building with GRYCE_HAS_BOX2D=ON for stable 2D physics.");
        return std::make_unique<BuiltinPhysicsWorld2D>();
    }
    GLOG_WARN("create_physics_world_2d: unknown backend '{}', falling back to builtin", backend_name);
    GLOG_WARN("create_physics_world_2d: using builtin physics (unstable, may cause errors). "
              "Consider building with GRYCE_HAS_BOX2D=ON for stable 2D physics.");
    return std::make_unique<BuiltinPhysicsWorld2D>();
}

std::unique_ptr<IPhysicsWorld3D> create_physics_world_3d(const std::string& backend_name) {
    // 默认优先 Jolt；只有显式传入 "builtin" 才使用自研实现
    if (backend_name == "jolt" || backend_name.empty()) {
#ifdef GRYCE_HAS_JOLT
        return std::make_unique<JoltPhysicsWorld3D>();
#else
        if (backend_name == "jolt") {
            GLOG_WARN("create_physics_world_3d: Jolt requested but not available, falling back to builtin");
        }
        GLOG_WARN("create_physics_world_3d: Jolt not available, using builtin physics (unstable, may cause errors). "
                  "Build with -DGRYCE_FETCH_JOLT=ON to enable Jolt.");
        return std::make_unique<BuiltinPhysicsWorld3D>();
#endif
    }
    if (backend_name == "builtin") {
        GLOG_WARN("create_physics_world_3d: using builtin physics (unstable, may cause errors). "
                  "Known issues: no CCD, unstable rotation, limited shape support.");
        return std::make_unique<BuiltinPhysicsWorld3D>();
    }
    GLOG_WARN("create_physics_world_3d: unknown backend '{}', falling back to builtin", backend_name);
    GLOG_WARN("create_physics_world_3d: using builtin physics (unstable, may cause errors). "
              "Known issues: no CCD, unstable rotation, limited shape support.");
    return std::make_unique<BuiltinPhysicsWorld3D>();
}

} // namespace gryce_engine::physics
