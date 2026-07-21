#pragma once

#include <string>
#include <variant>

#include "math/math.h"
#include "render/render2d.h"

namespace gryce_engine::editor {

// ---------------------------------------------------------------------------
// FieldValue — 反射字段值的类型擦除容器
// 覆盖 reflection::FieldType 支持的所有可编辑类型（Enum 用 int 存储）。
// ---------------------------------------------------------------------------
using FieldValue = std::variant<
    int,
    float,
    double,
    bool,
    std::string,
    math::Vector2f,
    math::Vector3f,
    math::Vector3i,
    math::Vector4f,
    math::Quaternionf,
    render::Color>;

} // namespace gryce_engine::editor
