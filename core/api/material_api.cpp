#include "GryceCore/material_api.h"
#include "internal_state.h"

#include "components/mesh_renderer.h"
#include "components/skinned_mesh_renderer.h"
#include "render/material.h"
#include "scene/entity.h"

#include <cstring>
#include <functional>
#include <string>

using gryce_engine::components::MeshRenderer;
using gryce_engine::components::SkinnedMeshRenderer;
using gryce_engine::render::Material;
using gryce_engine::scene::Entity;

namespace {

// 通过组件类型哈希找到 MeshRenderer 或 SkinnedMeshRenderer（二者都有 material）。
struct MaterialOwner {
    Entity* entity = nullptr;
    Material* material = nullptr;
    bool is_mesh = false;
};

MaterialOwner find_material_owner(GEntityHandle entity_handle, uint64_t comp_hash,
                                  bool create_if_missing) {
    MaterialOwner out;
    Entity* e = gryce_core::EntityResolver::resolve(entity_handle);
    if (!e) return out;
    out.entity = e;

    for (const auto& comp : e->components()) {
        std::string type_name = gryce_core::get_component_type_name(comp.get());
        if (std::hash<std::string>{}(type_name) != comp_hash) continue;

        if (auto* mr = dynamic_cast<MeshRenderer*>(comp.get())) {
            out.is_mesh = true;
            out.material = create_if_missing ? mr->ensure_material() : mr->material.get();
            return out;
        }
        if (auto* smr = dynamic_cast<SkinnedMeshRenderer*>(comp.get())) {
            out.is_mesh = false;
            out.material = create_if_missing ? smr->ensure_material() : smr->material.get();
            return out;
        }
        return out; // 组件存在但不是渲染器
    }
    return out;
}

struct FieldSpec {
    int float_count;
    bool is_string;
    bool is_bool;
    bool is_int;
};

FieldSpec spec_of(int field) {
    switch (field) {
        case GMAT_ALBEDO_COLOR:
        case GMAT_EMISSIVE_COLOR: return {3, false, false, false};
        case GMAT_UV_SCALE:
        case GMAT_UV_OFFSET:      return {2, false, false, false};
        case GMAT_BLEND_MODE:     return {1, false, false, true};
        case GMAT_TWO_SIDED:
        case GMAT_USE_ALBEDO_MAP:
        case GMAT_USE_NORMAL_MAP:
        case GMAT_USE_ROUGHNESS_MAP:
        case GMAT_USE_METALLIC_MAP:
        case GMAT_USE_AO_MAP:
        case GMAT_USE_EMISSIVE_MAP: return {1, false, true, false};
        case GMAT_ALBEDO_MAP_PATH:
        case GMAT_NORMAL_MAP_PATH:
        case GMAT_ROUGHNESS_MAP_PATH:
        case GMAT_METALLIC_MAP_PATH:
        case GMAT_AO_MAP_PATH:
        case GMAT_EMISSIVE_MAP_PATH: return {0, true, false, false};
        default: return {1, false, false, false}; // roughness/metallic/ao/opacity
    }
}

} // namespace

extern "C" {

int GMaterial_GetField(GEntityHandle entity, uint64_t comp_type_hash,
                       int field, float* out_floats, int float_capacity,
                       char* out_str, int str_capacity) {
    if (field < 0 || field >= GMAT_COUNT) return -1;
    auto owner = find_material_owner(entity, comp_type_hash, /*create_if_missing=*/false);
    if (!owner.material) return -1;

    FieldSpec spec = spec_of(field);
    Material& m = *owner.material;

    if (spec.is_string) {
        if (!out_str || str_capacity <= 0) return -1;
        const std::string* s = nullptr;
        switch (field) {
            case GMAT_ALBEDO_MAP_PATH:    s = &m.albedo_map_path; break;
            case GMAT_NORMAL_MAP_PATH:    s = &m.normal_map_path; break;
            case GMAT_ROUGHNESS_MAP_PATH: s = &m.roughness_map_path; break;
            case GMAT_METALLIC_MAP_PATH:  s = &m.metallic_map_path; break;
            case GMAT_AO_MAP_PATH:        s = &m.ao_map_path; break;
            case GMAT_EMISSIVE_MAP_PATH:  s = &m.emissive_map_path; break;
            default: return -1;
        }
        std::strncpy(out_str, s->c_str(), static_cast<size_t>(str_capacity) - 1);
        out_str[str_capacity - 1] = '\0';
        return static_cast<int>(std::strlen(out_str));
    }

    if (!out_floats || float_capacity < spec.float_count) return -1;
    switch (field) {
        case GMAT_ALBEDO_COLOR:  out_floats[0] = m.albedo_color.x;  out_floats[1] = m.albedo_color.y;  out_floats[2] = m.albedo_color.z; break;
        case GMAT_EMISSIVE_COLOR:out_floats[0] = m.emissive_color.x;out_floats[1] = m.emissive_color.y; out_floats[2] = m.emissive_color.z; break;
        case GMAT_UV_SCALE:      out_floats[0] = m.uv_scale.x;      out_floats[1] = m.uv_scale.y;      break;
        case GMAT_UV_OFFSET:     out_floats[0] = m.uv_offset.x;     out_floats[1] = m.uv_offset.y;     break;
        case GMAT_ROUGHNESS:     out_floats[0] = m.roughness; break;
        case GMAT_METALLIC:      out_floats[0] = m.metallic;  break;
        case GMAT_AO:            out_floats[0] = m.ao;        break;
        case GMAT_OPACITY:       out_floats[0] = m.opacity;   break;
        case GMAT_BLEND_MODE:    out_floats[0] = static_cast<float>(m.blend_mode); break;
        case GMAT_TWO_SIDED:     out_floats[0] = m.two_sided ? 1.0f : 0.0f; break;
        case GMAT_USE_ALBEDO_MAP:    out_floats[0] = m.use_albedo_map ? 1.0f : 0.0f; break;
        case GMAT_USE_NORMAL_MAP:    out_floats[0] = m.use_normal_map ? 1.0f : 0.0f; break;
        case GMAT_USE_ROUGHNESS_MAP: out_floats[0] = m.use_roughness_map ? 1.0f : 0.0f; break;
        case GMAT_USE_METALLIC_MAP:  out_floats[0] = m.use_metallic_map ? 1.0f : 0.0f; break;
        case GMAT_USE_AO_MAP:        out_floats[0] = m.use_ao_map ? 1.0f : 0.0f; break;
        case GMAT_USE_EMISSIVE_MAP:  out_floats[0] = m.use_emissive_map ? 1.0f : 0.0f; break;
        default: return -1;
    }
    return 0;
}

int GMaterial_SetField(GEntityHandle entity, uint64_t comp_type_hash,
                       int field, const float* in_floats, int float_count,
                       const char* in_str) {
    if (field < 0 || field >= GMAT_COUNT) return -1;
    auto owner = find_material_owner(entity, comp_type_hash, /*create_if_missing=*/true);
    if (!owner.material) return -1;

    FieldSpec spec = spec_of(field);
    Material& m = *owner.material;
    const bool affects_textures = field >= GMAT_ALBEDO_MAP_PATH;

    if (spec.is_string) {
        if (!in_str) return -1;
        switch (field) {
            case GMAT_ALBEDO_MAP_PATH:    m.albedo_map_path = in_str; break;
            case GMAT_NORMAL_MAP_PATH:    m.normal_map_path = in_str; break;
            case GMAT_ROUGHNESS_MAP_PATH: m.roughness_map_path = in_str; break;
            case GMAT_METALLIC_MAP_PATH:  m.metallic_map_path = in_str; break;
            case GMAT_AO_MAP_PATH:        m.ao_map_path = in_str; break;
            case GMAT_EMISSIVE_MAP_PATH:  m.emissive_map_path = in_str; break;
            default: return -1;
        }
        m.textures_dirty = true;
        return 0;
    }

    if (!in_floats || float_count < spec.float_count) return -1;
    switch (field) {
        case GMAT_ALBEDO_COLOR:  m.albedo_color = {in_floats[0], in_floats[1], in_floats[2]}; break;
        case GMAT_EMISSIVE_COLOR:m.emissive_color = {in_floats[0], in_floats[1], in_floats[2]}; break;
        case GMAT_UV_SCALE:      m.uv_scale = {in_floats[0], in_floats[1]}; break;
        case GMAT_UV_OFFSET:     m.uv_offset = {in_floats[0], in_floats[1]}; break;
        case GMAT_ROUGHNESS:     m.roughness = in_floats[0]; break;
        case GMAT_METALLIC:      m.metallic = in_floats[0]; break;
        case GMAT_AO:            m.ao = in_floats[0]; break;
        case GMAT_OPACITY:       m.opacity = in_floats[0]; break;
        case GMAT_BLEND_MODE:    m.blend_mode = in_floats[0] > 0.5f ? Material::BlendMode::Blend : Material::BlendMode::Opaque; break;
        case GMAT_TWO_SIDED:     m.two_sided = in_floats[0] > 0.5f; break;
        case GMAT_USE_ALBEDO_MAP:    m.use_albedo_map = in_floats[0] > 0.5f; break;
        case GMAT_USE_NORMAL_MAP:    m.use_normal_map = in_floats[0] > 0.5f; break;
        case GMAT_USE_ROUGHNESS_MAP: m.use_roughness_map = in_floats[0] > 0.5f; break;
        case GMAT_USE_METALLIC_MAP:  m.use_metallic_map = in_floats[0] > 0.5f; break;
        case GMAT_USE_AO_MAP:        m.use_ao_map = in_floats[0] > 0.5f; break;
        case GMAT_USE_EMISSIVE_MAP:  m.use_emissive_map = in_floats[0] > 0.5f; break;
        default: return -1;
    }
    if (affects_textures) m.textures_dirty = true;
    return 0;
}

} // extern "C"
