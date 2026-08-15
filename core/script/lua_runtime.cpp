#include "script/lua_runtime.h"

#include "GryceCore/types.h"
#include "runtime/engine_context.h"
#include "assets/asset_manager.h"
#include "components/2d/light_2d.h"
#include "components/2d/parallax_background.h"
#include "components/2d/particle_emitter.h"
#include "components/audio_source.h"
#include "components/box_collider_2d.h"
#include "components/circle_collider_2d.h"
#include "components/component_factory.h"
#include "components/script_component.h"
#include "components/transform.h"
#include "ecs/world.h"
#include "reflection/reflection.h"
#include "resources/project.h"
#include "scene/entity.h"
#include "scene/scene.h"

// Lua headers do not self-wrap with extern "C"; keep C linkage so the static
// library symbols (compiled as C) resolve from this C++ TU.
extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}
#include "utils/glog/glog_lib.h"

#include <cstring>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace gryce_engine::script {

namespace {

using gryce_engine::math::Vector2f;
using gryce_engine::math::Vector3f;
using gryce_engine::math::Vector4f;
using gryce_engine::math::Quaternionf;

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
scene::Scene* current_scene() {
    return LuaRuntime::instance().current_scene();
}

scene::Entity* resolve_entity(lua_State* L, int idx) {
    const auto h = static_cast<int>(luaL_checkinteger(L, idx));
    return LuaRuntime::instance().entity_by_handle(h);
}

int push_entity_handle(lua_State* L, scene::Entity* e) {
    if (!e) {
        lua_pushinteger(L, 0);
        return 1;
    }
    lua_pushinteger(L, LuaRuntime::instance().entity_handle(e));
    return 1;
}

static bool read_vec3(lua_State* L, int idx, Vector3f& out) {
    if (!lua_istable(L, idx)) return false;
    lua_getfield(L, idx, "x");
    lua_getfield(L, idx, "y");
    lua_getfield(L, idx, "z");
    out.x = static_cast<float>(lua_tonumber(L, -3));
    out.y = static_cast<float>(lua_tonumber(L, -2));
    out.z = static_cast<float>(lua_tonumber(L, -1));
    lua_pop(L, 3);
    return true;
}

static void push_vec3(lua_State* L, const Vector3f& v) {
    lua_createtable(L, 0, 3);
    lua_pushnumber(L, v.x); lua_setfield(L, -2, "x");
    lua_pushnumber(L, v.y); lua_setfield(L, -2, "y");
    lua_pushnumber(L, v.z); lua_setfield(L, -2, "z");
}

static void push_quat(lua_State* L, const Quaternionf& q) {
    lua_createtable(L, 0, 4);
    lua_pushnumber(L, q.x); lua_setfield(L, -2, "x");
    lua_pushnumber(L, q.y); lua_setfield(L, -2, "y");
    lua_pushnumber(L, q.z); lua_setfield(L, -2, "z");
    lua_pushnumber(L, q.w); lua_setfield(L, -2, "w");
}

static bool read_vec4(lua_State* L, int idx, Vector4f& out) {
    if (!lua_istable(L, idx)) return false;
    lua_getfield(L, idx, "x");
    lua_getfield(L, idx, "y");
    lua_getfield(L, idx, "z");
    lua_getfield(L, idx, "w");
    out.x = static_cast<float>(lua_tonumber(L, -4));
    out.y = static_cast<float>(lua_tonumber(L, -3));
    out.z = static_cast<float>(lua_tonumber(L, -2));
    out.w = static_cast<float>(lua_tonumber(L, -1));
    lua_pop(L, 4);
    return true;
}

static void push_vec4(lua_State* L, const Vector4f& v) {
    lua_createtable(L, 0, 4);
    lua_pushnumber(L, v.x); lua_setfield(L, -2, "x");
    lua_pushnumber(L, v.y); lua_setfield(L, -2, "y");
    lua_pushnumber(L, v.z); lua_setfield(L, -2, "z");
    lua_pushnumber(L, v.w); lua_setfield(L, -2, "w");
}

static bool read_color(lua_State* L, int idx, render::Color& out) {
    if (!lua_istable(L, idx)) return false;
    lua_getfield(L, idx, "r");
    lua_getfield(L, idx, "g");
    lua_getfield(L, idx, "b");
    lua_getfield(L, idx, "a");
    out.r = static_cast<float>(lua_tonumber(L, -4));
    out.g = static_cast<float>(lua_tonumber(L, -3));
    out.b = static_cast<float>(lua_tonumber(L, -2));
    out.a = static_cast<float>(lua_tonumber(L, -1));
    lua_pop(L, 4);
    return true;
}

static void push_color(lua_State* L, const render::Color& c) {
    lua_createtable(L, 0, 4);
    lua_pushnumber(L, c.r); lua_setfield(L, -2, "r");
    lua_pushnumber(L, c.g); lua_setfield(L, -2, "g");
    lua_pushnumber(L, c.b); lua_setfield(L, -2, "b");
    lua_pushnumber(L, c.a); lua_setfield(L, -2, "a");
}

// nlohmann::json -> Lua 值
static void push_json(lua_State* L, const nlohmann::json& j) {
    if (j.is_object()) {
        lua_newtable(L);
        for (auto it = j.begin(); it != j.end(); ++it) {
            push_json(L, it.value());
            lua_setfield(L, -2, it.key().c_str());
        }
    } else if (j.is_array()) {
        lua_newtable(L);
        int i = 1;
        for (const auto& v : j) {
            push_json(L, v);
            lua_rawseti(L, -2, i++);
        }
    } else if (j.is_string()) {
        const std::string s = j.get<std::string>();
        lua_pushlstring(L, s.c_str(), s.size());
    } else if (j.is_boolean()) {
        lua_pushboolean(L, j.get<bool>());
    } else if (j.is_number_integer()) {
        lua_pushinteger(L, static_cast<lua_Integer>(j.get<int64_t>()));
    } else if (j.is_number()) {
        lua_pushnumber(L, j.get<double>());
    } else {
        lua_pushnil(L);
    }
}

// ---------------------------------------------------------------------------
// engine.log
// ---------------------------------------------------------------------------
int l_log_info(lua_State* L) {
    const char* msg = luaL_optstring(L, 1, "");
    GLOG_INFO("{}", msg ? msg : "");
    return 0;
}

int l_log_warn(lua_State* L) {
    const char* msg = luaL_optstring(L, 1, "");
    GLOG_WARN("{}", msg ? msg : "");
    return 0;
}

int l_log_error(lua_State* L) {
    const char* msg = luaL_optstring(L, 1, "");
    GLOG_ERROR("{}", msg ? msg : "");
    return 0;
}

const luaL_Reg kLogLib[] = {
    {"info",  l_log_info},
    {"warn",  l_log_warn},
    {"error", l_log_error},
    {nullptr, nullptr}
};

// ---------------------------------------------------------------------------
// engine.*
// ---------------------------------------------------------------------------
int l_version(lua_State* L) {
    lua_pushfstring(L, "GryceSRT 0.2.0 (%s)", LUA_RELEASE);
    return 1;
}

int l_self(lua_State* L);  // defined below with the entity helpers

const luaL_Reg kEngineLib[] = {
    {"version", l_version},
    {"self", l_self},
    {nullptr, nullptr}
};

// ---------------------------------------------------------------------------
// engine.entity
// ---------------------------------------------------------------------------
// engine.self() -> entity handle of the currently running script
int l_self(lua_State* L) {
    auto* e = LuaRuntime::instance().current_entity();
    return push_entity_handle(L, e);
}

// engine.entity.get_name(h) -> string
int l_entity_get_name(lua_State* L) {
    auto* e = resolve_entity(L, 1);
    lua_pushstring(L, e ? e->name().c_str() : "");
    return 1;
}

// engine.entity.find(name) -> handle (0 if not found)
int l_entity_find(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    auto* scene = current_scene();
    if (!scene || !name) {
        lua_pushinteger(L, 0);
        return 1;
    }
    auto* e = scene->find_entity_by_name(name);
    return push_entity_handle(L, e);
}

// engine.entity.find_all(prefix) -> { handle, ... }
int l_entity_find_all(lua_State* L) {
    const char* prefix = luaL_checkstring(L, 1);
    auto* scene = current_scene();
    lua_newtable(L);
    if (!scene || !prefix) return 1;
    const size_t plen = std::strlen(prefix);
    int n = 1;
    scene->foreach([&](scene::Entity* e) {
        if (!e) return;
        const std::string& name = e->name();
        if (name == prefix ||
            (name.size() > plen && name.rfind(prefix, 0) == 0 &&
             std::isdigit(static_cast<unsigned char>(name[plen])))) {
            push_entity_handle(L, e);
            lua_rawseti(L, -2, n++);
        }
    });
    return 1;
}

// engine.entity.create(name) -> handle (立即创建，脚本遍历期间安全)
int l_entity_create(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    auto* scene = current_scene();
    if (!scene) {
        lua_pushinteger(L, 0);
        return 1;
    }
    scene::Entity* e = scene->create_entity(name ? name : "Entity");
    return push_entity_handle(L, e);
}

// engine.entity.destroy(h) -> bool（延迟到本帧脚本遍历结束后销毁）
int l_entity_destroy(lua_State* L) {
    auto* e = resolve_entity(L, 1);
    if (!e) {
        lua_pushboolean(L, false);
        return 1;
    }
    LuaRuntime::instance().queue_destroy(e);
    lua_pushboolean(L, true);
    return 1;
}

// engine.entity.aabb(h) -> {x, y, w, h}（中心 + 尺寸；优先取碰撞盒）
int l_entity_aabb(lua_State* L) {
    auto* e = resolve_entity(L, 1);
    if (!e || !e->transform()) {
        lua_pushnil(L);
        return 1;
    }
    const math::Vector3f& pos = e->transform()->position;
    const math::Vector3f& scl = e->transform()->scale;
    float w = 28.0f * scl.x;
    float h = 28.0f * scl.y;
    if (auto* box = e->get_component<components::BoxCollider2D>()) {
        w = box->size.x * scl.x;
        h = box->size.y * scl.y;
    } else if (auto* circle = e->get_component<components::CircleCollider2D>()) {
        w = h = circle->radius * 2.0f * std::max(std::abs(scl.x), std::abs(scl.y));
    }
    lua_createtable(L, 0, 4);
    lua_pushnumber(L, pos.x); lua_setfield(L, -2, "x");
    lua_pushnumber(L, pos.y); lua_setfield(L, -2, "y");
    lua_pushnumber(L, w);     lua_setfield(L, -2, "w");
    lua_pushnumber(L, h);     lua_setfield(L, -2, "h");
    return 1;
}

// engine.entity.get_transform(h) -> {pos={x,y,z}, rot={x,y,z,w}, scale={x,y,z}}
int l_entity_get_transform(lua_State* L) {
    auto* e = resolve_entity(L, 1);
    if (!e || !e->transform()) {
        lua_pushnil(L);
        return 1;
    }
    auto* t = e->transform();
    lua_createtable(L, 0, 3);
    push_vec3(L, t->position);
    lua_setfield(L, -2, "pos");
    push_quat(L, t->rotation);
    lua_setfield(L, -2, "rot");
    push_vec3(L, t->scale);
    lua_setfield(L, -2, "scale");
    return 1;
}

// engine.entity.set_transform(h, {pos={x,y,z}}, {rot={x,y,z,w}}, {scale={x,y,z}})
int l_entity_set_transform(lua_State* L) {
    auto* e = resolve_entity(L, 1);
    if (!e || !e->transform()) return 0;
    auto* t = e->transform();
    Vector3f pos, scl;
    Quaternionf rot;
    if (read_vec3(L, 2, pos)) t->position = pos;
    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "x"); lua_getfield(L, 3, "y");
        lua_getfield(L, 3, "z"); lua_getfield(L, 3, "w");
        rot.x = static_cast<float>(lua_tonumber(L, -4));
        rot.y = static_cast<float>(lua_tonumber(L, -3));
        rot.z = static_cast<float>(lua_tonumber(L, -2));
        rot.w = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 4);
        t->rotation = rot;
    }
    if (read_vec3(L, 4, scl)) t->scale = scl;
    e->mark_dirty();
    return 0;
}

const luaL_Reg kEntityLib[] = {
    {"get_name", l_entity_get_name},
    {"find", l_entity_find},
    {"find_all", l_entity_find_all},
    {"create", l_entity_create},
    {"destroy", l_entity_destroy},
    {"aabb", l_entity_aabb},
    {"get_transform", l_entity_get_transform},
    {"set_transform", l_entity_set_transform},
    {nullptr, nullptr}
};

// ---------------------------------------------------------------------------
// engine.component
// ---------------------------------------------------------------------------
// engine.component.has(h, type) -> bool
int l_component_has(lua_State* L) {
    auto* e = resolve_entity(L, 1);
    const char* type_name = luaL_checkstring(L, 2);
    lua_pushboolean(L, e && type_name && e->get_component_by_type(type_name) != nullptr);
    return 1;
}

// engine.component.get(h, type_name, prop_name) -> value
int l_component_get(lua_State* L) {
    auto* e = resolve_entity(L, 1);
    const char* type_name = luaL_checkstring(L, 2);
    const char* prop_name = luaL_checkstring(L, 3);
    if (!e || !type_name || !prop_name) {
        lua_pushnil(L);
        return 1;
    }

    auto* comp = e->get_component_by_type(type_name);
    if (!comp) {
        lua_pushnil(L);
        return 1;
    }
    auto fields = gryce_engine::reflection::Registry::instance().all_fields(
        gryce_core::reflection_lookup_name(type_name));
    for (const auto* f : fields) {
        if (f->name != prop_name || !f->read) continue;
        switch (f->type) {
            case gryce_engine::reflection::FieldType::Float: {
                float v = 0.0f; f->read(comp, &v); lua_pushnumber(L, v); return 1;
            }
            case gryce_engine::reflection::FieldType::Double: {
                double v = 0.0; f->read(comp, &v); lua_pushnumber(L, v); return 1;
            }
            case gryce_engine::reflection::FieldType::Int:
            case gryce_engine::reflection::FieldType::Enum: {
                int v = 0; f->read(comp, &v); lua_pushinteger(L, v); return 1;
            }
            case gryce_engine::reflection::FieldType::Bool: {
                bool v = false; f->read(comp, &v); lua_pushboolean(L, v); return 1;
            }
            case gryce_engine::reflection::FieldType::String: {
                std::string v; f->read(comp, &v);
                lua_pushlstring(L, v.c_str(), v.size()); return 1;
            }
            case gryce_engine::reflection::FieldType::Vector2f: {
                Vector2f v; f->read(comp, &v);
                lua_createtable(L, 0, 2);
                lua_pushnumber(L, v.x); lua_setfield(L, -2, "x");
                lua_pushnumber(L, v.y); lua_setfield(L, -2, "y");
                return 1;
            }
            case gryce_engine::reflection::FieldType::Vector3f: {
                Vector3f v; f->read(comp, &v);
                push_vec3(L, v);
                return 1;
            }
            case gryce_engine::reflection::FieldType::Vector4f: {
                Vector4f v; f->read(comp, &v);
                push_vec4(L, v);
                return 1;
            }
            case gryce_engine::reflection::FieldType::Color: {
                render::Color v; f->read(comp, &v);
                push_color(L, v);
                return 1;
            }
            case gryce_engine::reflection::FieldType::Quaternionf: {
                Quaternionf v; f->read(comp, &v);
                push_quat(L, v);
                return 1;
            }
            default:
                lua_pushnil(L);
                return 1;
        }
    }
    lua_pushnil(L);
    return 1;
}

// engine.component.set(h, type_name, prop_name, value)
// 组件不存在时自动创建（ComponentFactory）；ParallaxBackground 支持 tint 特殊字段。
int l_component_set(lua_State* L) {
    auto* e = resolve_entity(L, 1);
    const char* type_name = luaL_checkstring(L, 2);
    const char* prop_name = luaL_checkstring(L, 3);
    if (!e || !type_name || !prop_name) return 0;

    // ParallaxBackground 的 tint：应用到所有视差层（反射不支持嵌套结构）
    if (std::strcmp(type_name, "ParallaxBackground") == 0 &&
        std::strcmp(prop_name, "tint") == 0) {
        if (auto* pb = e->get_component<components::d2::parallax::ParallaxBackground>()) {
            render::Color c;
            if (read_color(L, 4, c)) {
                for (auto& layer : pb->layers) layer.tint = c;
                e->mark_dirty();
            }
        }
        return 0;
    }

    auto* comp = e->get_component_by_type(type_name);
    if (!comp) {
        auto created = components::ComponentFactory::instance().create(type_name);
        if (!created) return 0;
        comp = e->add_component(std::move(created));
        e->mark_dirty();
    }
    auto fields = gryce_engine::reflection::Registry::instance().all_fields(
        gryce_core::reflection_lookup_name(type_name));
    for (const auto* f : fields) {
        if (f->name != prop_name || !f->write || f->read_only) continue;
        switch (f->type) {
            case gryce_engine::reflection::FieldType::Float: {
                float v = static_cast<float>(luaL_checknumber(L, 4));
                if (f->write(comp, &v)) e->mark_dirty();
                return 0;
            }
            case gryce_engine::reflection::FieldType::Double: {
                double v = static_cast<double>(luaL_checknumber(L, 4));
                if (f->write(comp, &v)) e->mark_dirty();
                return 0;
            }
            case gryce_engine::reflection::FieldType::Int:
            case gryce_engine::reflection::FieldType::Enum: {
                int v = static_cast<int>(luaL_checkinteger(L, 4));
                if (f->write(comp, &v)) e->mark_dirty();
                return 0;
            }
            case gryce_engine::reflection::FieldType::Bool: {
                bool v = lua_toboolean(L, 4) != 0;
                if (f->write(comp, &v)) e->mark_dirty();
                return 0;
            }
            case gryce_engine::reflection::FieldType::String: {
                const char* s = luaL_checkstring(L, 4);
                std::string v = s ? s : "";
                if (f->write(comp, &v)) e->mark_dirty();
                return 0;
            }
            case gryce_engine::reflection::FieldType::Vector3f: {
                Vector3f v;
                if (read_vec3(L, 4, v)) {
                    if (f->write(comp, &v)) e->mark_dirty();
                }
                return 0;
            }
            case gryce_engine::reflection::FieldType::Vector2f: {
                Vector2f v;
                if (lua_istable(L, 4)) {
                    lua_getfield(L, 4, "x");
                    lua_getfield(L, 4, "y");
                    v.x = static_cast<float>(lua_tonumber(L, -2));
                    v.y = static_cast<float>(lua_tonumber(L, -1));
                    lua_pop(L, 2);
                    if (f->write(comp, &v)) e->mark_dirty();
                }
                return 0;
            }
            case gryce_engine::reflection::FieldType::Vector4f: {
                Vector4f v;
                if (read_vec4(L, 4, v)) {
                    if (f->write(comp, &v)) e->mark_dirty();
                }
                return 0;
            }
            case gryce_engine::reflection::FieldType::Color: {
                render::Color v;
                if (read_color(L, 4, v)) {
                    if (f->write(comp, &v)) e->mark_dirty();
                }
                return 0;
            }
            case gryce_engine::reflection::FieldType::Quaternionf: {
                Quaternionf v;
                if (lua_istable(L, 4)) {
                    lua_getfield(L, 4, "x"); lua_getfield(L, 4, "y");
                    lua_getfield(L, 4, "z"); lua_getfield(L, 4, "w");
                    v.x = static_cast<float>(lua_tonumber(L, -4));
                    v.y = static_cast<float>(lua_tonumber(L, -3));
                    v.z = static_cast<float>(lua_tonumber(L, -2));
                    v.w = static_cast<float>(lua_tonumber(L, -1));
                    lua_pop(L, 4);
                    if (f->write(comp, &v)) e->mark_dirty();
                }
                return 0;
            }
            default:
                return 0;
        }
    }
    return 0;
}

const luaL_Reg kComponentLib[] = {
    {"has", l_component_has},
    {"get", l_component_get},
    {"set", l_component_set},
    {nullptr, nullptr}
};

// ---------------------------------------------------------------------------
// engine.state（跨实体/跨场景共享状态表）
// ---------------------------------------------------------------------------
int l_state_get(lua_State* L) {
    const char* key = luaL_checkstring(L, 1);
    const int ref = LuaRuntime::instance().state_table_ref();
    if (ref < 0 || !key) {
        lua_pushnil(L);
        return 1;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    lua_getfield(L, -1, key);
    lua_remove(L, -2);
    return 1;
}

int l_state_set(lua_State* L) {
    const char* key = luaL_checkstring(L, 1);
    const int ref = LuaRuntime::instance().state_table_ref();
    if (ref < 0 || !key) return 0;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    lua_pushvalue(L, 2);
    lua_setfield(L, -2, key);
    lua_pop(L, 1);
    return 0;
}

int l_state_has(lua_State* L) {
    const char* key = luaL_checkstring(L, 1);
    const int ref = LuaRuntime::instance().state_table_ref();
    if (ref < 0 || !key) {
        lua_pushboolean(L, false);
        return 1;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    lua_getfield(L, -1, key);
    lua_pushboolean(L, !lua_isnil(L, -1));
    lua_pop(L, 2);
    return 1;
}

const luaL_Reg kStateLib[] = {
    {"get", l_state_get},
    {"set", l_state_set},
    {"has", l_state_has},
    {nullptr, nullptr}
};

// ---------------------------------------------------------------------------
// engine.time
// ---------------------------------------------------------------------------
int l_time_delta(lua_State* L) {
    lua_pushnumber(L, LuaRuntime::instance().delta());
    return 1;
}

int l_time_elapsed(lua_State* L) {
    lua_pushnumber(L, LuaRuntime::instance().elapsed());
    return 1;
}

const luaL_Reg kTimeLib[] = {
    {"delta", l_time_delta},
    {"elapsed", l_time_elapsed},
    {nullptr, nullptr}
};

// ---------------------------------------------------------------------------
// engine.input
// ---------------------------------------------------------------------------
int l_input_key_down(lua_State* L) {
    const int key = static_cast<int>(luaL_checkinteger(L, 1));
    const bool down = gryce_core::g_core_state.input.keys_down.count(key) > 0;
    lua_pushboolean(L, down);
    return 1;
}

int l_input_mouse_pos(lua_State* L) {
    lua_pushinteger(L, gryce_core::g_core_state.input.mouse_x);
    lua_pushinteger(L, gryce_core::g_core_state.input.mouse_y);
    return 2;
}

int l_input_mouse_down(lua_State* L) {
    const int b = static_cast<int>(luaL_checkinteger(L, 1));
    const bool down = b >= 0 && b < 3 && gryce_core::g_core_state.input.mouse_button[b];
    lua_pushboolean(L, down);
    return 1;
}

int l_input_mouse_delta(lua_State* L) {
    lua_pushnumber(L, gryce_core::g_core_state.input.mouse_delta_x);
    lua_pushnumber(L, gryce_core::g_core_state.input.mouse_delta_y);
    return 2;
}

int l_input_mouse_locked(lua_State* L) {
    auto& st = gryce_core::g_core_state;
    // 传 bool 时请求锁定/解锁，并转发给平台回调（隐藏/锁定光标）
    if (lua_gettop(L) == 1 && lua_isboolean(L, 1)) {
        const bool locked = lua_toboolean(L, 1);
        st.input.mouse_locked = locked;
        if (st.callbacks.on_mouse_lock) {
            st.callbacks.on_mouse_lock(locked ? 1 : 0, st.callback_user_data);
        }
    }
    lua_pushboolean(L, st.input.mouse_locked);
    return 1;
}

const luaL_Reg kInputLib[] = {
    {"key_down", l_input_key_down},
    {"mouse_pos", l_input_mouse_pos},
    {"mouse_down", l_input_mouse_down},
    {"mouse_delta", l_input_mouse_delta},
    {"mouse_locked", l_input_mouse_locked},
    {nullptr, nullptr}
};

// ---------------------------------------------------------------------------
// engine.signal
// ---------------------------------------------------------------------------
// engine.signal.connect(name, target_handle, callback) -> bool
// 把当前脚本的某个信号连接到目标实体脚本环境里的回调函数。
int l_signal_connect(lua_State* L) {
    const char* sig_name = luaL_checkstring(L, 1);
    scene::Entity* target_e = resolve_entity(L, 2);
    if (!sig_name || !target_e) {
        lua_pushboolean(L, false);
        return 1;
    }
    auto* target_comp = target_e->get_component<components::ScriptComponent>();
    if (!target_comp || target_comp->env_ref < 0) {
        lua_pushboolean(L, false);
        return 1;
    }
    if (!lua_isfunction(L, 3)) {
        lua_pushboolean(L, false);
        return 1;
    }
    lua_pushvalue(L, 3);
    const int callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    auto* self_comp = LuaRuntime::instance().current_entity()
                          ? LuaRuntime::instance().current_entity()->get_component<components::ScriptComponent>()
                          : nullptr;
    if (self_comp) {
        self_comp->signals.push_back({
            std::string(sig_name),
            LuaRuntime::instance().entity_handle(target_e),
            target_comp->env_ref,
            callback_ref});
        lua_pushboolean(L, true);
    } else {
        luaL_unref(L, LUA_REGISTRYINDEX, callback_ref);
        lua_pushboolean(L, false);
    }
    return 1;
}

// engine.signal.emit(name, ...) -> bool
// 触发当前脚本上的同名信号，依次调用所有已连接的回调。
int l_signal_emit(lua_State* L) {
    const char* sig_name = luaL_checkstring(L, 1);
    auto* self_comp = LuaRuntime::instance().current_entity()
                          ? LuaRuntime::instance().current_entity()->get_component<components::ScriptComponent>()
                          : nullptr;
    if (!self_comp) {
        lua_pushboolean(L, false);
        return 1;
    }
    const int arg_count = lua_gettop(L) - 1;
    for (const auto& sig : self_comp->signals) {
        if (sig.name != sig_name || sig.callback_ref < 0) continue;
        scene::Entity* target_e = LuaRuntime::instance().entity_by_handle(sig.target_handle);
        if (target_e) LuaRuntime::instance().set_current_entity(target_e);
        lua_rawgeti(L, LUA_REGISTRYINDEX, sig.callback_ref);   // cb
        for (int i = 0; i < arg_count; ++i) {
            lua_pushvalue(L, i + 2);                            // cb, args...
        }
        if (lua_pcall(L, arg_count, 0, 0) != LUA_OK) {
            const char* msg = lua_tostring(L, -1);
            GLOG_ERROR("GryceSRT: signal '{}' callback error: {}", sig_name, msg ? msg : "unknown");
            lua_pop(L, 1);                                      // error
        }
        if (target_e) LuaRuntime::instance().set_current_entity(self_comp->owner());
    }
    lua_pushboolean(L, true);
    return 1;
}

const luaL_Reg kSignalLib[] = {
    {"connect", l_signal_connect},
    {"emit", l_signal_emit},
    {nullptr, nullptr}
};

// ---------------------------------------------------------------------------
// engine.scene
// ---------------------------------------------------------------------------
// engine.scene.load(path) -> 0 on success, -1 on failure.
int l_scene_load(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    if (!path || !path[0]) {
        lua_pushinteger(L, -1);
        return 1;
    }
    if (!gryce_core::g_core_state.initialized || !gryce_core::g_core_state.world) {
        lua_pushinteger(L, -1);
        return 1;
    }
    GCommand cmd{};
    cmd.type = ECMD_LOAD_SCENE;
    std::strncpy(reinterpret_cast<char*>(cmd.payload), path, GCMD_PAYLOAD_SIZE - 1);
    const bool ok = gryce_core::g_core_state.cmdbuf.push(cmd);
    lua_pushinteger(L, ok ? 0 : -1);
    return 1;
}

// engine.scene.current() -> res:/ path of the active scene, or nil.
int l_scene_current(lua_State* L) {
    const std::string& p = gryce_core::g_core_state.current_scene_path;
    if (p.empty()) {
        lua_pushnil(L);
    } else {
        lua_pushstring(L, p.c_str());
    }
    return 1;
}

const luaL_Reg kSceneLib[] = {
    {"load", l_scene_load},
    {"current", l_scene_current},
    {nullptr, nullptr}
};

// ---------------------------------------------------------------------------
// engine.audio
// ---------------------------------------------------------------------------
// engine.audio.play_on(h) -> bool（播放实体上的 AudioSource）
int l_audio_play_on(lua_State* L) {
    auto* e = resolve_entity(L, 1);
    if (!e) {
        lua_pushboolean(L, false);
        return 1;
    }
    auto* src = e->get_component<components::AudioSource>();
    if (!src) {
        lua_pushboolean(L, false);
        return 1;
    }
    src->play();
    lua_pushboolean(L, true);
    return 1;
}

const luaL_Reg kAudioLib[] = {
    {"play_on", l_audio_play_on},
    {nullptr, nullptr}
};

// ---------------------------------------------------------------------------
// engine.fx
// ---------------------------------------------------------------------------
// engine.fx.burst(h) -> bool（粒子爆发一次）
int l_fx_burst(lua_State* L) {
    auto* e = resolve_entity(L, 1);
    if (!e) {
        lua_pushboolean(L, false);
        return 1;
    }
    auto* pe = e->get_component<components::d2::ParticleEmitter2D>();
    if (!pe) {
        lua_pushboolean(L, false);
        return 1;
    }
    pe->burst();
    lua_pushboolean(L, true);
    return 1;
}

const luaL_Reg kFxLib[] = {
    {"burst", l_fx_burst},
    {nullptr, nullptr}
};

// ---------------------------------------------------------------------------
// engine.json
// ---------------------------------------------------------------------------
// engine.json.read(path) -> table | nil（读取项目内 JSON，支持从 .gpkg 提取）
int l_json_read(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    if (!path) {
        lua_pushnil(L);
        return 1;
    }
    const std::string resolved =
        assets::AssetManager::instance().resolve_for_reading(path);
    if (resolved.empty()) {
        lua_pushnil(L);
        return 1;
    }
    std::ifstream in(resolved, std::ios::binary);
    if (!in) {
        lua_pushnil(L);
        return 1;
    }
    try {
        nlohmann::json j;
        in >> j;
        push_json(L, j);
    } catch (const std::exception&) {
        lua_pushnil(L);
    }
    return 1;
}

const luaL_Reg kJsonLib[] = {
    {"read", l_json_read},
    {nullptr, nullptr}
};

// ---------------------------------------------------------------------------
// engine.physics
// ---------------------------------------------------------------------------
// engine.physics.set_gravity(x, y)
int l_physics_set_gravity(lua_State* L) {
    const float x = static_cast<float>(luaL_checknumber(L, 1));
    const float y = static_cast<float>(luaL_checknumber(L, 2));
    if (gryce_core::g_core_state.world) {
        if (auto* sys = gryce_core::g_core_state.world->get_system("PhysicsSystem2D")) {
            sys->set_gravity(Vector2f(x, y));
        }
    }
    return 0;
}

// engine.physics.get_gravity() -> x, y | nil
int l_physics_get_gravity(lua_State* L) {
    if (gryce_core::g_core_state.world) {
        if (auto* sys = gryce_core::g_core_state.world->get_system("PhysicsSystem2D")) {
            const Vector2f g = sys->get_gravity();
            lua_pushnumber(L, g.x);
            lua_pushnumber(L, g.y);
            return 2;
        }
    }
    lua_pushnil(L);
    return 1;
}

const luaL_Reg kPhysicsLib[] = {
    {"set_gravity", l_physics_set_gravity},
    {"get_gravity", l_physics_get_gravity},
    {nullptr, nullptr}
};

// ---------------------------------------------------------------------------
// require 的后备 searcher：从 res:/scripts/<name>.lua 加载模块
// （通过 AssetManager 解析，打包产物里脚本位于 .gpkg 内也能 require）。
// ---------------------------------------------------------------------------
int l_package_bundle_searcher(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    if (!name) {
        lua_pushliteral(L, "\n\tinvalid module name");
        return 1;
    }
    const std::string path = "res:/scripts/" + std::string(name) + ".lua";
    const std::string resolved =
        assets::AssetManager::instance().resolve_for_reading(path);
    if (resolved.empty()) {
        lua_pushstring(L, ("\n\tno bundle script '" + path + "'").c_str());
        return 1;
    }
    std::ifstream in(resolved, std::ios::binary);
    if (!in) {
        lua_pushstring(L, "\n\tcannot open bundle script");
        return 1;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string src = ss.str();
    if (luaL_loadbuffer(L, src.c_str(), src.size(), path.c_str()) != LUA_OK) {
        return 1;  // 语法错误信息留在栈上，交给 require 报错
    }
    return 1;  // loader 函数
}

} // namespace

LuaRuntime& LuaRuntime::instance() {
    static LuaRuntime rt;
    return rt;
}

bool LuaRuntime::init() {
    if (L_) return true;

    L_ = luaL_newstate();
    if (!L_) {
        GLOG_ERROR("GryceSRT: failed to create Lua state");
        return false;
    }

    luaL_openlibs(L_);

    // engine.state 共享状态表（跨实体/跨场景）
    lua_newtable(L_);
    state_table_ref_ = luaL_ref(L_, LUA_REGISTRYINDEX);

    register_engine_bindings();
    GLOG_INFO("GryceSRT: Lua runtime initialized ({})", LUA_RELEASE);
    return true;
}

void LuaRuntime::shutdown() {
    if (L_) {
        if (state_table_ref_ >= 0) {
            luaL_unref(L_, LUA_REGISTRYINDEX, state_table_ref_);
            state_table_ref_ = -1;
        }
        lua_close(L_);
        L_ = nullptr;
        current_scene_ = nullptr;
        entity_handles_.clear();
        entities_by_handle_.clear();
        pending_destroy_.clear();
        GLOG_INFO("GryceSRT: Lua runtime shut down");
    }
}

bool LuaRuntime::run_string(const char* code, std::string* err) {
    if (!L_ || !code) return false;

    if (luaL_loadstring(L_, code) != LUA_OK) {
        if (err) {
            const char* e = lua_tostring(L_, -1);
            *err = e ? e : "unknown Lua compile error";
        }
        lua_pop(L_, 1);
        return false;
    }

    if (lua_pcall(L_, 0, 0, 0) != LUA_OK) {
        if (err) {
            const char* e = lua_tostring(L_, -1);
            *err = e ? e : "unknown Lua runtime error";
        }
        lua_pop(L_, 1);
        return false;
    }
    return true;
}

bool LuaRuntime::run_file(const char* path, std::string* err) {
    if (!L_ || !path) return false;

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (err) *err = std::string("cannot open script file: ") + path;
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return run_string(ss.str().c_str(), err);
}

void LuaRuntime::set_current_scene(scene::Scene* scene) {
    if (current_scene_ == scene) return;
    current_scene_ = scene;
    entity_handles_.clear();
    entities_by_handle_.clear();
    pending_destroy_.clear();
}

int LuaRuntime::entity_handle(scene::Entity* e) {
    if (!e) return 0;
    auto it = entity_handles_.find(e->uuid());
    if (it != entity_handles_.end()) return it->second;
    const int h = next_handle_++;
    entity_handles_[e->uuid()] = h;
    entities_by_handle_[h] = e;
    return h;
}

scene::Entity* LuaRuntime::entity_by_handle(int h) const {
    auto it = entities_by_handle_.find(h);
    return it != entities_by_handle_.end() ? it->second : nullptr;
}

void LuaRuntime::queue_destroy(scene::Entity* e) {
    if (e) pending_destroy_.push_back(e);
}

std::vector<scene::Entity*> LuaRuntime::take_pending_destroy() {
    std::vector<scene::Entity*> out;
    out.swap(pending_destroy_);
    return out;
}

void LuaRuntime::register_engine_bindings() {
    lua_getglobal(L_, "engine");
    if (lua_isnil(L_, -1)) {
        lua_pop(L_, 1);
        lua_newtable(L_);
        lua_setglobal(L_, "engine");
        lua_getglobal(L_, "engine");
    }

    lua_newtable(L_);
    luaL_setfuncs(L_, kLogLib, 0);
    lua_setfield(L_, -2, "log");

    lua_newtable(L_);
    luaL_setfuncs(L_, kEntityLib, 0);
    lua_setfield(L_, -2, "entity");

    lua_newtable(L_);
    luaL_setfuncs(L_, kComponentLib, 0);
    lua_setfield(L_, -2, "component");

    lua_newtable(L_);
    luaL_setfuncs(L_, kStateLib, 0);
    lua_setfield(L_, -2, "state");

    lua_newtable(L_);
    luaL_setfuncs(L_, kTimeLib, 0);
    lua_setfield(L_, -2, "time");

    lua_newtable(L_);
    luaL_setfuncs(L_, kInputLib, 0);
    // 暴露输入事件类型常量，供 _input(type, ...) 分支判断。
    lua_pushinteger(L_, gryce_core::INPUT_EVENT_KEY_DOWN);
    lua_setfield(L_, -2, "KEY_DOWN");
    lua_pushinteger(L_, gryce_core::INPUT_EVENT_KEY_UP);
    lua_setfield(L_, -2, "KEY_UP");
    lua_pushinteger(L_, gryce_core::INPUT_EVENT_MOUSE_MOVE);
    lua_setfield(L_, -2, "MOUSE_MOVE");
    lua_pushinteger(L_, gryce_core::INPUT_EVENT_MOUSE_DOWN);
    lua_setfield(L_, -2, "MOUSE_DOWN");
    lua_pushinteger(L_, gryce_core::INPUT_EVENT_MOUSE_UP);
    lua_setfield(L_, -2, "MOUSE_UP");
    lua_setfield(L_, -2, "input");

    lua_newtable(L_);
    luaL_setfuncs(L_, kSceneLib, 0);
    lua_setfield(L_, -2, "scene");

    lua_newtable(L_);
    luaL_setfuncs(L_, kSignalLib, 0);
    lua_setfield(L_, -2, "signal");

    lua_newtable(L_);
    luaL_setfuncs(L_, kAudioLib, 0);
    lua_setfield(L_, -2, "audio");

    lua_newtable(L_);
    luaL_setfuncs(L_, kFxLib, 0);
    lua_setfield(L_, -2, "fx");

    lua_newtable(L_);
    luaL_setfuncs(L_, kJsonLib, 0);
    lua_setfield(L_, -2, "json");

    lua_newtable(L_);
    luaL_setfuncs(L_, kPhysicsLib, 0);
    lua_setfield(L_, -2, "physics");

    luaL_setfuncs(L_, kEngineLib, 0);
    lua_pop(L_, 1);

    // Make require("GryceEngineUtils") resolve against the project's
    // scripts/ folder (project root is set before the runtime inits).
    const std::string root = resources::Project::instance().root();
    if (!root.empty()) {
        const std::string scripts = root + "/scripts/?.lua";
        lua_getglobal(L_, "package");
        lua_getfield(L_, -1, "path");
        const char* old = lua_tostring(L_, -1);
        const std::string new_path = scripts + ";" + (old ? old : "");
        lua_pop(L_, 1);
        lua_pushstring(L_, new_path.c_str());
        lua_setfield(L_, -2, "path");

        // 追加 bundle searcher（先走标准 package.path，找不到再查资源包）
        lua_getfield(L_, -1, "searchers");
        const int n = static_cast<int>(lua_rawlen(L_, -1));
        lua_pushcfunction(L_, l_package_bundle_searcher);
        lua_rawseti(L_, -2, n + 1);
        lua_pop(L_, 2);
    }
}

} // namespace gryce_engine::script
