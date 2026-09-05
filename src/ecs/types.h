#pragma once

#include <cstdint>
#include <typeindex>
#include <string>

namespace gryce_engine::ecs {

// ---------------------------------------------------------------------------
// ECS 基础类型
// ---------------------------------------------------------------------------
using EntityID = uint64_t;
using ComponentTypeID = std::type_index;

constexpr EntityID k_invalid_entity = 0;

} // namespace gryce_engine::ecs
