// ===========================================================================
// engine/script/js_bindings.cpp
// engine.* 原生绑定实现。API 面与口径见 js_bindings.h。
//
// 输入状态来源：gryce_core::g_input_state（由 ECMD_INPUT_* 命令更新、
// GCore_BeginFrame 帧起点清理 input_events，宿主契约见
// engine/runtime/engine_context.h 与 api/core_api.cpp）。
// ===========================================================================

#include "script/js_bindings.h"

#include <cstring>

#include "runtime/engine_context.h"
#include "scene/entity.h"
#include "script/js_value.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::script {

namespace {

ScriptHost* host_of(JSContext* ctx) {
    return static_cast<ScriptHost*>(JS_GetContextOpaque(ctx));
}

scene::Entity* require_host_entity(JSContext* ctx) {
    auto* host = host_of(ctx);
    if (!host) return nullptr;
    return host->host_entity();
}

// ---------------------------------------------------------------------------
// engine.log.*
// ---------------------------------------------------------------------------
JSValue js_log_info(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    if (argc < 1) return throw_type_error(ctx, "engine.log.info(msg) expects 1 argument");
    std::string msg;
    from_js(ctx, argv[0], msg);
    GLOG_INFO("[script] {}", msg);
    return JS_UNDEFINED;
}

JSValue js_log_warn(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    if (argc < 1) return throw_type_error(ctx, "engine.log.warn(msg) expects 1 argument");
    std::string msg;
    from_js(ctx, argv[0], msg);
    GLOG_WARN("[script] {}", msg);
    return JS_UNDEFINED;
}

JSValue js_log_error(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    if (argc < 1) return throw_type_error(ctx, "engine.log.error(msg) expects 1 argument");
    std::string msg;
    from_js(ctx, argv[0], msg);
    GLOG_ERROR("[script] {}", msg);
    return JS_UNDEFINED;
}

JSValue js_log_debug(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    if (argc < 1) return throw_type_error(ctx, "engine.log.debug(msg) expects 1 argument");
    std::string msg;
    from_js(ctx, argv[0], msg);
    GLOG_DEBUG("[script] {}", msg);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// engine.time.*
// ---------------------------------------------------------------------------
JSValue js_time_dt(JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/, JSValueConst* /*argv*/) {
    auto* host = host_of(ctx);
    return to_js(ctx, host ? host->host_dt() : 0.0f);
}

JSValue js_time_frame(JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/, JSValueConst* /*argv*/) {
    auto* host = host_of(ctx);
    return to_js(ctx, static_cast<int64_t>(host ? host->host_frame() : 0));
}

JSValue js_time_elapsed(JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/, JSValueConst* /*argv*/) {
    auto* host = host_of(ctx);
    return to_js(ctx, host ? host->host_elapsed() : 0.0f);
}

// ---------------------------------------------------------------------------
// engine.input.*（读 g_input_state）
// ---------------------------------------------------------------------------
JSValue js_input_key_down(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    if (argc < 1) return throw_type_error(ctx, "engine.input.key_down(code) expects 1 argument");
    int32_t code = 0;
    from_js(ctx, argv[0], code);
    return to_js(ctx, gryce_core::g_input_state.keys_down.contains(code));
}

JSValue js_input_key_up(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    if (argc < 1) return throw_type_error(ctx, "engine.input.key_up(code) expects 1 argument");
    int32_t code = 0;
    from_js(ctx, argv[0], code);
    return to_js(ctx, !gryce_core::g_input_state.keys_down.contains(code));
}

JSValue js_input_mouse_x(JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/,
                         JSValueConst* /*argv*/) {
    return to_js(ctx, gryce_core::g_input_state.mouse_x);
}

JSValue js_input_mouse_y(JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/,
                         JSValueConst* /*argv*/) {
    return to_js(ctx, gryce_core::g_input_state.mouse_y);
}

JSValue js_input_mouse_delta(JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/,
                             JSValueConst* /*argv*/) {
    const auto& in = gryce_core::g_input_state;
    return to_js(ctx, math::Vector2f(in.mouse_delta_x, in.mouse_delta_y));
}

JSValue js_input_mouse_button(JSContext* ctx, JSValueConst /*this_val*/, int argc,
                              JSValueConst* argv) {
    if (argc < 1) return throw_type_error(ctx, "engine.input.mouse_button(i) expects 1 argument");
    int32_t i = 0;
    from_js(ctx, argv[0], i);
    const bool down = (i >= 0 && i < 3) ? gryce_core::g_input_state.mouse_button[i] : false;
    return to_js(ctx, down);
}

JSValue js_input_mouse_locked(JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/,
                              JSValueConst* /*argv*/) {
    return to_js(ctx, gryce_core::g_input_state.mouse_locked);
}

// ---------------------------------------------------------------------------
// engine.entity.*（宿主实体；owner 为空时抛错）
// ---------------------------------------------------------------------------
JSValue js_entity_self(JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/,
                       JSValueConst* /*argv*/) {
    (void)ctx;
    // v1 句柄表只有宿主自身一项
    return JS_NewInt32(ctx, 1);
}

JSValue js_entity_name(JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/,
                       JSValueConst* /*argv*/) {
    auto* e = require_host_entity(ctx);
    if (!e) return throw_type_error(ctx, "engine.entity: no host entity");
    return to_js(ctx, e->name());
}

JSValue js_entity_set_name(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    auto* e = require_host_entity(ctx);
    if (!e) return throw_type_error(ctx, "engine.entity: no host entity");
    if (argc < 1) return throw_type_error(ctx, "engine.entity.set_name(s) expects 1 argument");
    std::string s;
    from_js(ctx, argv[0], s);
    e->set_name(s);
    return JS_UNDEFINED;
}

JSValue js_entity_position(JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/,
                           JSValueConst* /*argv*/) {
    auto* e = require_host_entity(ctx);
    if (!e) return throw_type_error(ctx, "engine.entity: no host entity");
    return to_js(ctx, e->transform()->position);
}

JSValue js_entity_set_position(JSContext* ctx, JSValueConst /*this_val*/, int argc,
                               JSValueConst* argv) {
    auto* e = require_host_entity(ctx);
    if (!e) return throw_type_error(ctx, "engine.entity: no host entity");
    math::Vector3f p;
    if (argc >= 1 && !from_js(ctx, argv[0], p)) {
        return throw_type_error(ctx, "engine.entity.set_position([x,y,z])");
    }
    if (argc >= 3) {
        from_js(ctx, argv[0], p.x);
        from_js(ctx, argv[1], p.y);
        from_js(ctx, argv[2], p.z);
    }
    e->transform()->position = p;
    return JS_UNDEFINED;
}

JSValue js_entity_rotation(JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/,
                           JSValueConst* /*argv*/) {
    auto* e = require_host_entity(ctx);
    if (!e) return throw_type_error(ctx, "engine.entity: no host entity");
    return to_js(ctx, e->transform()->rotation);
}

JSValue js_entity_set_rotation(JSContext* ctx, JSValueConst /*this_val*/, int argc,
                               JSValueConst* argv) {
    auto* e = require_host_entity(ctx);
    if (!e) return throw_type_error(ctx, "engine.entity: no host entity");
    math::Quaternionf q;
    if (argc >= 1 && !from_js(ctx, argv[0], q)) {
        return throw_type_error(ctx, "engine.entity.set_rotation([x,y,z,w])");
    }
    if (argc >= 4) {
        from_js(ctx, argv[0], q.x);
        from_js(ctx, argv[1], q.y);
        from_js(ctx, argv[2], q.z);
        from_js(ctx, argv[3], q.w);
    }
    e->transform()->rotation = q;
    return JS_UNDEFINED;
}

JSValue js_entity_scale(JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/,
                        JSValueConst* /*argv*/) {
    auto* e = require_host_entity(ctx);
    if (!e) return throw_type_error(ctx, "engine.entity: no host entity");
    return to_js(ctx, e->transform()->scale);
}

JSValue js_entity_set_scale(JSContext* ctx, JSValueConst /*this_val*/, int argc,
                            JSValueConst* argv) {
    auto* e = require_host_entity(ctx);
    if (!e) return throw_type_error(ctx, "engine.entity: no host entity");
    math::Vector3f s;
    if (argc >= 1 && !from_js(ctx, argv[0], s)) {
        return throw_type_error(ctx, "engine.entity.set_scale([x,y,z])");
    }
    if (argc >= 3) {
        from_js(ctx, argv[0], s.x);
        from_js(ctx, argv[1], s.y);
        from_js(ctx, argv[2], s.z);
    }
    e->transform()->scale = s;
    return JS_UNDEFINED;
}

JSValue js_entity_translate(JSContext* ctx, JSValueConst /*this_val*/, int argc,
                            JSValueConst* argv) {
    auto* e = require_host_entity(ctx);
    if (!e) return throw_type_error(ctx, "engine.entity: no host entity");
    math::Vector3f d;
    if (argc >= 1 && !from_js(ctx, argv[0], d)) {
        return throw_type_error(ctx, "engine.entity.translate([dx,dy,dz])");
    }
    if (argc >= 3) {
        from_js(ctx, argv[0], d.x);
        from_js(ctx, argv[1], d.y);
        from_js(ctx, argv[2], d.z);
    }
    e->transform()->position = e->transform()->position + d;
    return JS_UNDEFINED;
}

JSValue js_entity_set_enabled(JSContext* ctx, JSValueConst /*this_val*/, int argc,
                              JSValueConst* argv) {
    auto* e = require_host_entity(ctx);
    if (!e) return throw_type_error(ctx, "engine.entity: no host entity");
    if (argc < 1) return throw_type_error(ctx, "engine.entity.set_enabled(bool) expects 1 argument");
    bool b = false;
    from_js(ctx, argv[0], b);
    e->enabled = b;
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// 安装
// ---------------------------------------------------------------------------
void set_fn(JSContext* ctx, JSValue obj, const char* name, JSCFunction* fn, int argc) {
    JSValue f = JS_NewCFunction(ctx, fn, name, argc);
    JS_SetPropertyStr(ctx, obj, name, f);
}

// ---------------------------------------------------------------------------
// console.*（浏览器 / Node 习惯）：多参数按空格拼接后转发 glog，
// 便于从网页脚本直接迁移；语义与 engine.log.* 相同。
// ---------------------------------------------------------------------------
std::string join_console_args(JSContext* ctx, int argc, JSValueConst* argv) {
    std::string out;
    for (int i = 0; i < argc; ++i) {
        std::string part;
        if (!from_js(ctx, argv[i], part)) {
            // 对象 / 数组等非标量：退回 JSON 文本，避免打印出空串
            part = from_js(ctx, argv[i]).dump();
        }
        if (i) out += ' ';
        out += part;
    }
    return out;
}

JSValue js_console_log(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    GLOG_INFO("[script] {}", join_console_args(ctx, argc, argv));
    return JS_UNDEFINED;
}

JSValue js_console_warn(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    GLOG_WARN("[script] {}", join_console_args(ctx, argc, argv));
    return JS_UNDEFINED;
}

JSValue js_console_error(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    GLOG_ERROR("[script] {}", join_console_args(ctx, argc, argv));
    return JS_UNDEFINED;
}

JSValue js_console_debug(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    GLOG_DEBUG("[script] {}", join_console_args(ctx, argc, argv));
    return JS_UNDEFINED;
}

} // namespace

void install_engine_bindings(JSContext* ctx, ScriptHost* host) {
    JS_SetContextOpaque(ctx, host);

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue engine = JS_NewObject(ctx);
    JSValue log_obj = JS_NewObject(ctx);
    set_fn(ctx, log_obj, "info", &js_log_info, 1);
    set_fn(ctx, log_obj, "warn", &js_log_warn, 1);
    set_fn(ctx, log_obj, "error", &js_log_error, 1);
    set_fn(ctx, log_obj, "debug", &js_log_debug, 1);
    JS_SetPropertyStr(ctx, engine, "log", log_obj);

    JSValue time_obj = JS_NewObject(ctx);
    set_fn(ctx, time_obj, "dt", &js_time_dt, 0);
    set_fn(ctx, time_obj, "frame", &js_time_frame, 0);
    set_fn(ctx, time_obj, "elapsed", &js_time_elapsed, 0);
    JS_SetPropertyStr(ctx, engine, "time", time_obj);

    JSValue input_obj = JS_NewObject(ctx);
    set_fn(ctx, input_obj, "key_down", &js_input_key_down, 1);
    set_fn(ctx, input_obj, "key_up", &js_input_key_up, 1);
    set_fn(ctx, input_obj, "mouse_x", &js_input_mouse_x, 0);
    set_fn(ctx, input_obj, "mouse_y", &js_input_mouse_y, 0);
    set_fn(ctx, input_obj, "mouse_delta", &js_input_mouse_delta, 0);
    set_fn(ctx, input_obj, "mouse_button", &js_input_mouse_button, 1);
    set_fn(ctx, input_obj, "mouse_locked", &js_input_mouse_locked, 0);
    JS_SetPropertyStr(ctx, engine, "input", input_obj);

    JSValue entity_obj = JS_NewObject(ctx);
    set_fn(ctx, entity_obj, "self", &js_entity_self, 0);
    set_fn(ctx, entity_obj, "name", &js_entity_name, 0);
    set_fn(ctx, entity_obj, "set_name", &js_entity_set_name, 1);
    set_fn(ctx, entity_obj, "position", &js_entity_position, 0);
    set_fn(ctx, entity_obj, "set_position", &js_entity_set_position, 1);
    set_fn(ctx, entity_obj, "rotation", &js_entity_rotation, 0);
    set_fn(ctx, entity_obj, "set_rotation", &js_entity_set_rotation, 1);
    set_fn(ctx, entity_obj, "scale", &js_entity_scale, 0);
    set_fn(ctx, entity_obj, "set_scale", &js_entity_set_scale, 1);
    set_fn(ctx, entity_obj, "translate", &js_entity_translate, 1);
    set_fn(ctx, entity_obj, "set_enabled", &js_entity_set_enabled, 1);
    JS_SetPropertyStr(ctx, engine, "entity", entity_obj);

    JS_SetPropertyStr(ctx, global, "engine", engine);

    // console.*：与 engine.log.* 同源（同一 glog 通道），多参数按空格拼接
    JSValue console_obj = JS_NewObject(ctx);
    set_fn(ctx, console_obj, "log", &js_console_log, 1);
    set_fn(ctx, console_obj, "warn", &js_console_warn, 1);
    set_fn(ctx, console_obj, "error", &js_console_error, 1);
    set_fn(ctx, console_obj, "debug", &js_console_debug, 1);
    JS_SetPropertyStr(ctx, global, "console", console_obj);

    JS_FreeValue(ctx, global);
}

} // namespace gryce_engine::script
