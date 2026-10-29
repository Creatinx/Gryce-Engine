// ===========================================================================
// engine/script/js_app_bindings.cpp
// 入口脚本绑定实现。API 面与口径见 js_app_bindings.h。
//
// 进程内只有一个应用入口，服务指针存在 TU 级静态变量里（不是 context
// opaque —— 那个位置已被 ScriptHost* 占用，见 js_bindings.cpp）。
// ===========================================================================

#include "script/js_app_bindings.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "api/core_api.h"
#include "api/entity_api.h"
#include "api/scene_api.h"
#include "api/types.h"
#include "runtime/engine_context.h"
#include "scene/entity.h"
#include "script/js_value.h"
#include "script/script_app.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::script {

namespace {

AppHostServices* g_services = nullptr;
ScriptApplication* g_app = nullptr;

void set_fn(JSContext* ctx, JSValue obj, const char* name, JSCFunction* fn, int argc) {
    JS_SetPropertyStr(ctx, obj, name, JS_NewCFunction(ctx, fn, name, argc));
}

// ---------------------------------------------------------------------------
// 实体句柄 <-> JS
// ---------------------------------------------------------------------------
GEntityHandle this_handle(JSContext* ctx, JSValueConst this_val, bool* ok) {
    JSValue id = JS_GetPropertyStr(ctx, this_val, "__id");
    int32_t h = 0;
    const bool good = from_js(ctx, id, h) && h > 0;
    JS_FreeValue(ctx, id);
    *ok = good;
    return good ? static_cast<GEntityHandle>(h) : 0;
}

bool arg_handle(JSContext* ctx, JSValueConst v, GEntityHandle* out) {
    // 实体代理对象或裸整数句柄都接受：destroy(e) 两种写法都通。
    if (JS_IsObject(v)) {
        JSValue id = JS_GetPropertyStr(ctx, v, "__id");
        int32_t h = 0;
        const bool ok = from_js(ctx, id, h) && h > 0;
        JS_FreeValue(ctx, id);
        if (ok) *out = static_cast<GEntityHandle>(h);
        return ok;
    }
    int32_t h = 0;
    if (!from_js(ctx, v, h) || h <= 0) return false;
    *out = static_cast<GEntityHandle>(h);
    return true;
}

// [x,y,z] 或 (x,y,z)
bool read_vec3(JSContext* ctx, int argc, JSValueConst* argv, math::Vector3f& out) {
    if (argc >= 3) {
        double x = 0.0, y = 0.0, z = 0.0;
        if (!from_js(ctx, argv[0], x) || !from_js(ctx, argv[1], y) ||
            !from_js(ctx, argv[2], z)) {
            return false;
        }
        out = math::Vector3f(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
        return true;
    }
    if (argc >= 1) return from_js(ctx, argv[0], out);
    return false;
}

bool read_quat(JSContext* ctx, int argc, JSValueConst* argv, math::Quaternionf& out) {
    if (argc >= 4) {
        double x = 0.0, y = 0.0, z = 0.0, w = 1.0;
        if (!from_js(ctx, argv[0], x) || !from_js(ctx, argv[1], y) ||
            !from_js(ctx, argv[2], z) || !from_js(ctx, argv[3], w)) {
            return false;
        }
        out = math::Quaternionf(static_cast<float>(x), static_cast<float>(y),
                                static_cast<float>(z), static_cast<float>(w));
        return true;
    }
    if (argc >= 1) return from_js(ctx, argv[0], out);
    return false;
}

JSValue make_entity_proxy(JSContext* ctx, GEntityHandle h);

// ---------------------------------------------------------------------------
// 实体代理方法
// ---------------------------------------------------------------------------
JSValue js_ent_id(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    bool ok = false;
    const GEntityHandle h = this_handle(ctx, this_val, &ok);
    if (!ok) return throw_type_error(ctx, "entity: invalid handle");
    return to_js(ctx, static_cast<int32_t>(h));
}

JSValue js_ent_valid(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    bool ok = false;
    const GEntityHandle h = this_handle(ctx, this_val, &ok);
    if (!ok) return JS_FALSE;
    return to_js(ctx, gryce_core::EntityResolver::resolve(h) != nullptr);
}

JSValue js_ent_name(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    bool ok = false;
    const GEntityHandle h = this_handle(ctx, this_val, &ok);
    if (!ok) return throw_type_error(ctx, "entity: invalid handle");
    char buf[256] = {};
    // 返回值是名字长度（成功）或 -1（实体失效 / 参数非法），不是错误码。
    if (GEntity_GetName(h, buf, static_cast<int>(sizeof(buf))) < 0) {
        return throw_type_error(ctx, "entity: get name failed");
    }
    return to_js(ctx, std::string(buf));
}

JSValue js_ent_set_name(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    bool ok = false;
    const GEntityHandle h = this_handle(ctx, this_val, &ok);
    if (!ok) return throw_type_error(ctx, "entity: invalid handle");
    if (argc < 1) return throw_type_error(ctx, "entity.set_name(s) expects 1 argument");
    std::string name;
    if (!from_js(ctx, argv[0], name)) return throw_type_error(ctx, "entity.set_name(s)");
    GCommand cmd{};
    cmd.type = ECMD_RENAME_ENTITY;
    struct Payload { GEntityHandle h; char name[128]; };
    Payload p{};
    p.h = h;
    std::snprintf(p.name, sizeof(p.name), "%s", name.c_str());
    std::memcpy(cmd.payload, &p, sizeof(p));
    if (GCore_PushCommand(&cmd) != 0) return throw_type_error(ctx, "entity.set_name: command queue full");
    return JS_UNDEFINED;
}

JSValue js_ent_position(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    bool ok = false;
    const GEntityHandle h = this_handle(ctx, this_val, &ok);
    if (!ok) return throw_type_error(ctx, "entity: invalid handle");
    GVec3 p{};
    if (GEntity_GetLocalPosition(h, &p) != 0) return throw_type_error(ctx, "entity.position failed");
    return to_js(ctx, math::Vector3f(p.x, p.y, p.z));
}

JSValue js_ent_set_position(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    bool ok = false;
    const GEntityHandle h = this_handle(ctx, this_val, &ok);
    if (!ok) return throw_type_error(ctx, "entity: invalid handle");
    math::Vector3f v;
    if (!read_vec3(ctx, argc, argv, v)) {
        return throw_type_error(ctx, "entity.set_position(x,y,z) or ([x,y,z])");
    }
    const GVec3 p{v.x, v.y, v.z};
    if (GEntity_SetLocalPosition(h, &p) != 0) return throw_type_error(ctx, "entity.set_position failed");
    return JS_UNDEFINED;
}

JSValue js_ent_rotation(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    bool ok = false;
    const GEntityHandle h = this_handle(ctx, this_val, &ok);
    if (!ok) return throw_type_error(ctx, "entity: invalid handle");
    GQuat q{};
    if (GEntity_GetLocalRotation(h, &q) != 0) return throw_type_error(ctx, "entity.rotation failed");
    return to_js(ctx, math::Quaternionf(q.x, q.y, q.z, q.w));
}

JSValue js_ent_set_rotation(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    bool ok = false;
    const GEntityHandle h = this_handle(ctx, this_val, &ok);
    if (!ok) return throw_type_error(ctx, "entity: invalid handle");
    math::Quaternionf q;
    if (!read_quat(ctx, argc, argv, q)) {
        return throw_type_error(ctx, "entity.set_rotation(x,y,z,w) or ([x,y,z,w])");
    }
    const GQuat gq{q.x, q.y, q.z, q.w};
    if (GEntity_SetLocalRotation(h, &gq) != 0) return throw_type_error(ctx, "entity.set_rotation failed");
    return JS_UNDEFINED;
}

JSValue js_ent_scale(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    bool ok = false;
    const GEntityHandle h = this_handle(ctx, this_val, &ok);
    if (!ok) return throw_type_error(ctx, "entity: invalid handle");
    GVec3 s{};
    if (GEntity_GetLocalScale(h, &s) != 0) return throw_type_error(ctx, "entity.scale failed");
    return to_js(ctx, math::Vector3f(s.x, s.y, s.z));
}

JSValue js_ent_set_scale(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    bool ok = false;
    const GEntityHandle h = this_handle(ctx, this_val, &ok);
    if (!ok) return throw_type_error(ctx, "entity: invalid handle");
    math::Vector3f v;
    if (!read_vec3(ctx, argc, argv, v)) {
        return throw_type_error(ctx, "entity.set_scale(x,y,z) or ([x,y,z])");
    }
    const GVec3 s{v.x, v.y, v.z};
    if (GEntity_SetLocalScale(h, &s) != 0) return throw_type_error(ctx, "entity.set_scale failed");
    return JS_UNDEFINED;
}

JSValue js_ent_translate(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    bool ok = false;
    const GEntityHandle h = this_handle(ctx, this_val, &ok);
    if (!ok) return throw_type_error(ctx, "entity: invalid handle");
    math::Vector3f d;
    if (!read_vec3(ctx, argc, argv, d)) {
        return throw_type_error(ctx, "entity.translate(dx,dy,dz) or ([dx,dy,dz])");
    }
    GVec3 p{};
    if (GEntity_GetLocalPosition(h, &p) != 0) return throw_type_error(ctx, "entity.translate failed");
    p.x += d.x;
    p.y += d.y;
    p.z += d.z;
    if (GEntity_SetLocalPosition(h, &p) != 0) return throw_type_error(ctx, "entity.translate failed");
    return JS_UNDEFINED;
}

JSValue js_ent_enabled(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    bool ok = false;
    const GEntityHandle h = this_handle(ctx, this_val, &ok);
    if (!ok) return throw_type_error(ctx, "entity: invalid handle");
    // 实体启用位没有独立 C API，借用命名接口判断存在性后读世界指针
    const auto* e = gryce_core::EntityResolver::resolve(h);
    if (!e) return throw_type_error(ctx, "entity: stale handle");
    return to_js(ctx, e->enabled);
}

JSValue js_ent_set_enabled(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    bool ok = false;
    const GEntityHandle h = this_handle(ctx, this_val, &ok);
    if (!ok) return throw_type_error(ctx, "entity: invalid handle");
    if (argc < 1) return throw_type_error(ctx, "entity.set_enabled(bool) expects 1 argument");
    bool b = true;
    from_js(ctx, argv[0], b);
    auto* e = gryce_core::EntityResolver::resolve(h);
    if (!e) return throw_type_error(ctx, "entity: stale handle");
    e->enabled = b;
    return JS_UNDEFINED;
}

JSValue js_ent_destroy(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    bool ok = false;
    const GEntityHandle h = this_handle(ctx, this_val, &ok);
    if (!ok) return throw_type_error(ctx, "entity: invalid handle");
    GCommand cmd{};
    cmd.type = ECMD_DESTROY_ENTITY;
    std::memcpy(cmd.payload, &h, sizeof(h));
    if (GCore_PushCommand(&cmd) != 0) return throw_type_error(ctx, "entity.destroy: command queue full");
    return JS_UNDEFINED;
}

JSValue make_entity_proxy(JSContext* ctx, GEntityHandle h) {
    if (h <= 0) return JS_NULL;
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "__id", JS_NewInt32(ctx, h));
    set_fn(ctx, obj, "id", &js_ent_id, 0);
    set_fn(ctx, obj, "valid", &js_ent_valid, 0);
    set_fn(ctx, obj, "name", &js_ent_name, 0);
    set_fn(ctx, obj, "set_name", &js_ent_set_name, 1);
    set_fn(ctx, obj, "position", &js_ent_position, 0);
    set_fn(ctx, obj, "set_position", &js_ent_set_position, 3);
    set_fn(ctx, obj, "rotation", &js_ent_rotation, 0);
    set_fn(ctx, obj, "set_rotation", &js_ent_set_rotation, 4);
    set_fn(ctx, obj, "scale", &js_ent_scale, 0);
    set_fn(ctx, obj, "set_scale", &js_ent_set_scale, 3);
    set_fn(ctx, obj, "translate", &js_ent_translate, 3);
    set_fn(ctx, obj, "enabled", &js_ent_enabled, 0);
    set_fn(ctx, obj, "set_enabled", &js_ent_set_enabled, 1);
    set_fn(ctx, obj, "destroy", &js_ent_destroy, 0);
    return obj;
}

// ---------------------------------------------------------------------------
// engine.entity.*
//
// 句柄枚举走 EntityHandleMap::all_handles()，不用 GEntity_GetCount/GetAt：
// 后两者只统计 parent() != nullptr 的实体（编辑器树视图口径），场景里的
// 根实体（本 demo 的相机/灯光/模型全是根）在那里根本不可见。
// ---------------------------------------------------------------------------
std::vector<GEntityHandle> all_entity_handles() {
    return gryce_core::g_core_state.entity_map.all_handles();
}

JSValue js_entity_count(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return to_js(ctx, static_cast<int32_t>(all_entity_handles().size()));
}

JSValue js_entity_find(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return throw_type_error(ctx, "engine.entity.find(name) expects 1 argument");
    std::string name;
    if (!from_js(ctx, argv[0], name)) return throw_type_error(ctx, "engine.entity.find(name)");
    for (const GEntityHandle h : all_entity_handles()) {
        const scene::Entity* e = gryce_core::EntityResolver::resolve(h);
        if (e && e->name() == name) return make_entity_proxy(ctx, h);
    }
    return JS_NULL;
}

JSValue js_entity_all(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    JSValue arr = JS_NewArray(ctx);
    uint32_t out = 0;
    for (const GEntityHandle h : all_entity_handles()) {
        JS_SetPropertyUint32(ctx, arr, out++, make_entity_proxy(ctx, h));
    }
    return arr;
}

JSValue js_entity_spawn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return throw_type_error(ctx, "engine.entity.spawn(json) expects 1 argument");
    std::string json;
    if (!from_js(ctx, argv[0], json)) return throw_type_error(ctx, "engine.entity.spawn(json)");
    const GEntityHandle h = GEntity_ImportJson(json.c_str(), 0);
    if (h == 0) return throw_type_error(ctx, "engine.entity.spawn: import failed");
    return make_entity_proxy(ctx, h);
}

JSValue js_entity_destroy(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return throw_type_error(ctx, "engine.entity.destroy(entity) expects 1 argument");
    GEntityHandle h = 0;
    if (!arg_handle(ctx, argv[0], &h)) return throw_type_error(ctx, "engine.entity.destroy(entity)");
    GCommand cmd{};
    cmd.type = ECMD_DESTROY_ENTITY;
    std::memcpy(cmd.payload, &h, sizeof(h));
    if (GCore_PushCommand(&cmd) != 0) return throw_type_error(ctx, "engine.entity.destroy: command queue full");
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// engine.scene.*
// ---------------------------------------------------------------------------
// engine.scene.load([path])
//   不传参 = 载入 project.data 的 main_scene（启动场景的唯一真源）；
//   传参   = 载入指定场景。脚本不要再写死场景路径，否则改配置等于没改。
JSValue js_scene_load(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!g_services) return throw_type_error(ctx, "engine.scene: no host services");
    std::string path;
    if (argc < 1 || JS_IsUndefined(argv[0]) || JS_IsNull(argv[0])) {
        path = g_services->main_scene();
        if (path.empty()) {
            return throw_type_error(ctx,
                                    "engine.scene.load(): project.data has no main_scene");
        }
    } else if (!from_js(ctx, argv[0], path)) {
        return throw_type_error(ctx, "engine.scene.load(path)");
    }
    const bool ok = g_services->load_scene(path);
    if (!ok) GLOG_ERROR("[script] engine.scene.load('{}') failed", path);
    return to_js(ctx, ok);
}

JSValue js_scene_current(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return to_js(ctx, g_services ? g_services->current_scene() : std::string{});
}

// ---------------------------------------------------------------------------
// engine.window.*
// ---------------------------------------------------------------------------
JSValue js_window_create(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!g_services) return throw_type_error(ctx, "engine.window: no host services");
    WindowOptions opts;
    if (argc >= 1 && JS_IsObject(argv[0])) {
        JSValue v = JS_GetPropertyStr(ctx, argv[0], "title");
        from_js(ctx, v, opts.title);
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[0], "width");
        from_js(ctx, v, opts.width);
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[0], "height");
        from_js(ctx, v, opts.height);
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[0], "api");
        from_js(ctx, v, opts.api);
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[0], "vsync");
        from_js(ctx, v, opts.vsync);
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[0], "resizable");
        from_js(ctx, v, opts.resizable);
        JS_FreeValue(ctx, v);
    }
    if (opts.width <= 0 || opts.height <= 0) {
        return throw_type_error(ctx, "engine.window.create: width/height must be positive");
    }
    if (!g_services->create_window(opts)) {
        GLOG_ERROR("[script] engine.window.create failed ({})", opts.api.empty() ? "auto" : opts.api);
        return JS_FALSE;
    }
    return JS_TRUE;
}

JSValue js_window_close(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    if (!g_services) return throw_type_error(ctx, "engine.window: no host services");
    g_services->destroy_window();
    return JS_UNDEFINED;
}

JSValue js_window_valid(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return to_js(ctx, g_services ? g_services->window_valid() : false);
}

JSValue js_window_size(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    int w = 0;
    int h = 0;
    if (g_services) g_services->window_size(&w, &h);
    JSValue arr = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, arr, 0, to_js(ctx, static_cast<int32_t>(w)));
    JS_SetPropertyUint32(ctx, arr, 1, to_js(ctx, static_cast<int32_t>(h)));
    return arr;
}

// ---------------------------------------------------------------------------
// engine.app.*
// ---------------------------------------------------------------------------
JSValue js_app_quit(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    if (!g_services) return throw_type_error(ctx, "engine.app: no host services");
    g_services->request_quit();
    return JS_UNDEFINED;
}

JSValue js_app_fps(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return to_js(ctx, g_services ? g_services->fps() : 0.0f);
}

JSValue js_app_has_window(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return to_js(ctx, g_services ? g_services->window_valid() : false);
}

JSValue js_app_has_renderer(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return to_js(ctx, g_services ? g_services->has_renderer() : false);
}

JSValue js_app_screenshot(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return throw_type_error(ctx, "engine.app.screenshot(path) expects 1 argument");
    std::string path;
    if (!from_js(ctx, argv[0], path)) return throw_type_error(ctx, "engine.app.screenshot(path)");
    if (!g_services) return throw_type_error(ctx, "engine.app: no host services");
    return to_js(ctx, g_services->screenshot(path));
}

// ---------------------------------------------------------------------------
// engine.input.set_mouse_locked(bool)
// ---------------------------------------------------------------------------
JSValue js_input_set_mouse_locked(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return throw_type_error(ctx, "engine.input.set_mouse_locked(bool)");
    bool locked = false;
    from_js(ctx, argv[0], locked);
    if (g_services) g_services->set_cursor_locked(locked);
    return JS_UNDEFINED;
}

} // namespace

// ---------------------------------------------------------------------------
// engine.render.* —— 渲染效果开关（DEMO 与编辑器在运行时对比不同反射方案）
// 通过 AppHostServices 反向注入，绑定层本身不链接渲染器。
// ---------------------------------------------------------------------------
namespace {

JSValue js_render_set_rt_reflection(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return throw_type_error(ctx, "engine.render.set_rt_reflection(bool)");
    bool v = false;
    from_js(ctx, argv[0], v);
    if (g_services) g_services->set_rt_reflection(v);
    return JS_UNDEFINED;
}
JSValue js_render_is_rt_reflection(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return to_js(ctx, g_services ? g_services->rt_reflection_enabled() : false);
}
JSValue js_render_set_ssr(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return throw_type_error(ctx, "engine.render.set_ssr(bool)");
    bool v = false;
    from_js(ctx, argv[0], v);
    if (g_services) g_services->set_ssr(v);
    return JS_UNDEFINED;
}
JSValue js_render_is_ssr(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return to_js(ctx, g_services ? g_services->ssr_enabled() : false);
}

} // namespace

void install_app_bindings(JSContext* ctx, ScriptApplication* app, AppHostServices* services) {
    g_services = services;
    g_app = app;
    (void)g_app;

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue engine = JS_GetPropertyStr(ctx, global, "engine");
    if (JS_IsUndefined(engine)) {
        engine = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "engine", engine);
    }

    JSValue app_obj = JS_NewObject(ctx);
    set_fn(ctx, app_obj, "quit", &js_app_quit, 0);
    set_fn(ctx, app_obj, "fps", &js_app_fps, 0);
    set_fn(ctx, app_obj, "has_window", &js_app_has_window, 0);
    set_fn(ctx, app_obj, "has_renderer", &js_app_has_renderer, 0);
    set_fn(ctx, app_obj, "screenshot", &js_app_screenshot, 1);
    JS_SetPropertyStr(ctx, engine, "app", app_obj);

    JSValue window_obj = JS_NewObject(ctx);
    set_fn(ctx, window_obj, "create", &js_window_create, 1);
    set_fn(ctx, window_obj, "close", &js_window_close, 0);
    set_fn(ctx, window_obj, "valid", &js_window_valid, 0);
    set_fn(ctx, window_obj, "size", &js_window_size, 0);
    JS_SetPropertyStr(ctx, engine, "window", window_obj);

    JSValue scene_obj = JS_NewObject(ctx);
    set_fn(ctx, scene_obj, "load", &js_scene_load, 1);
    set_fn(ctx, scene_obj, "current", &js_scene_current, 0);
    JS_SetPropertyStr(ctx, engine, "scene", scene_obj);

    JSValue entity_obj = JS_GetPropertyStr(ctx, engine, "entity");
    if (JS_IsUndefined(entity_obj)) {
        JS_FreeValue(ctx, entity_obj);
        entity_obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, engine, "entity", JS_DupValue(ctx, entity_obj));
    }
    set_fn(ctx, entity_obj, "count", &js_entity_count, 0);
    set_fn(ctx, entity_obj, "find", &js_entity_find, 1);
    set_fn(ctx, entity_obj, "all", &js_entity_all, 0);
    set_fn(ctx, entity_obj, "spawn", &js_entity_spawn, 1);
    set_fn(ctx, entity_obj, "destroy", &js_entity_destroy, 1);
    JS_FreeValue(ctx, entity_obj);

    JSValue input_obj = JS_GetPropertyStr(ctx, engine, "input");
    if (JS_IsObject(input_obj)) {
        set_fn(ctx, input_obj, "set_mouse_locked", &js_input_set_mouse_locked, 1);
    }
    JS_FreeValue(ctx, input_obj);

    JSValue render_obj = JS_NewObject(ctx);
    set_fn(ctx, render_obj, "set_rt_reflection", &js_render_set_rt_reflection, 1);
    set_fn(ctx, render_obj, "is_rt_reflection", &js_render_is_rt_reflection, 0);
    set_fn(ctx, render_obj, "set_ssr", &js_render_set_ssr, 1);
    set_fn(ctx, render_obj, "is_ssr", &js_render_is_ssr, 0);
    JS_SetPropertyStr(ctx, engine, "render", render_obj);

    JS_FreeValue(ctx, engine);
    JS_FreeValue(ctx, global);
}

} // namespace gryce_engine::script
