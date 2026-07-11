#pragma once

#include "physics/physics_world_2d.h"
#include "physics/physics_world_3d.h"
#include <memory>
#include <string>

namespace gryce_engine::physics {

std::unique_ptr<IPhysicsWorld2D> create_physics_world_2d(const std::string& backend_name);
std::unique_ptr<IPhysicsWorld3D> create_physics_world_3d(const std::string& backend_name);

} // namespace gryce_engine::physics
