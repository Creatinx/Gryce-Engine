#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "render/export.h"
#include "math/math.h"

namespace gryce_engine::render {

// 材质 RID 句柄
using MaterialRID = uint32_t;
constexpr MaterialRID k_invalid_material_rid = 0;

// 材质参数类型
enum class MaterialParamType {
    Float,
    Vec2,
    Vec3,
    Vec4,
    Texture,
    Color
};

// 材质参数
struct MaterialParam {
    std::string name;
    MaterialParamType type = MaterialParamType::Float;
    union {
        float float_value;
        uint32_t texture_rid;
    };
    math::Vector4f vec4_value = math::Vector4f(0, 0, 0, 0);
};

// ---------------------------------------------------------------------------
// RendererMaterialStorage — 材质存储抽象接口
// 管理材质参数、shader 参数绑定、uniform set。
// ---------------------------------------------------------------------------
class GRYCE_RENDERER_API RendererMaterialStorage {
public:
    virtual ~RendererMaterialStorage() = default;

    virtual MaterialRID material_create() = 0;
    virtual void material_free(MaterialRID rid) = 0;
    virtual void material_set_param(MaterialRID rid, const std::string& name,
                                    float value) = 0;
    virtual void material_set_param(MaterialRID rid, const std::string& name,
                                    const math::Vector3f& value) = 0;
    virtual void material_set_param(MaterialRID rid, const std::string& name,
                                    const math::Vector4f& value) = 0;
    virtual void material_set_texture(MaterialRID rid, const std::string& name,
                                      uint32_t texture_rid) = 0;
    virtual void material_set_shader(MaterialRID rid, uint32_t shader_rid) = 0;

    virtual void update_buffers() = 0;
};

} // namespace gryce_engine::render