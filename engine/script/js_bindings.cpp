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
#include "components/component.h"
#include "scene/entity.h"
#include "script/js_value.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::script {

namespace {

using gryce_engine::components::Component;

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
// engine.component.*（组件树与类型继承）
//
// 组件下标口径与 C API 完全一致：宿主实体上的可见组件序列
// （Entity::user_components()，过滤 Transform / ParentComponent /
//  ChildrenComponent）。脚本按下标取到的组件与编辑器按下标取到的组件
// 是同一个 —— 两侧共用 Entity::user_components() 这一处实现。
// ---------------------------------------------------------------------------

// 第 index 个可见组件；越界返回 nullptr
Component* component_at(JSContext* ctx, int index) {
    auto* host = host_of(ctx);
    if (!host) return nullptr;
    if (index < 0 || index >= host->host_component_count()) return nullptr;
    return host->host_component_at(index);
}

// 组件指针 → 可见下标（找不到返回 -1；父组件是内部组件时也会返回 -1）
int component_index_of(JSContext* ctx, Component* target) {
    auto* host = host_of(ctx);
    if (!host || !target) return -1;
    const int count = host->host_component_count();
    for (int i = 0; i < count; ++i) {
        if (host->host_component_at(i) == target) return i;
    }
    return -1;
}

// 类型全名 → 短名（取最后一个 "::" 之后的段）
std::string short_type_name(const std::string& full_name) {
    const size_t pos = full_name.rfind("::");
    if (pos != std::string::npos) return full_name.substr(pos + 2);
    return full_name;
}

JSValue js_component_count(JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/,
                           JSValueConst* /*argv*/) {
    auto* host = host_of(ctx);
    if (!host) return throw_type_error(ctx, "engine.component: no host");
    return to_js(ctx, host->host_component_count());
}

JSValue js_component_get_parent(JSContext* ctx, JSValueConst /*this_val*/, int argc,
                                 JSValueConst* argv) {
    if (argc < 1) return throw_type_error(ctx, "engine.component.getParent(index) expects 1 argument");
    int32_t index = -1;
    from_js(ctx, argv[0], index);
    Component* comp = component_at(ctx, index);
    if (!comp) return to_js(ctx, -1);
    Component* parent = comp->component_parent();
    if (!parent) return to_js(ctx, -1);
    return to_js(ctx, component_index_of(ctx, parent));
}

JSValue js_component_set_parent(JSContext* ctx, JSValueConst /*this_val*/, int argc,
                                 JSValueConst* argv) {
    if (argc < 2) {
        return throw_type_error(ctx, "engine.component.setParent(index, parentIndex) expects 2 arguments");
    }
    auto* e = require_host_entity(ctx);
    if (!e) return throw_type_error(ctx, "engine.component: no host entity");
    int32_t index = -1;
    int32_t parent_index = -1;
    from_js(ctx, argv[0], index);
    from_js(ctx, argv[1], parent_index);
    Component* child = component_at(ctx, index);
    if (!child) return to_js(ctx, false);
    Component* parent = nullptr;
    if (parent_index >= 0) {
        parent = component_at(ctx, parent_index);
        if (!parent) return to_js(ctx, false);
    }
    // 防环（挂到自己的后代之下）由 Entity::set_component_parent 兜底
    const bool ok = e->set_component_parent(child, parent);
    if (ok) e->mark_dirty();
    return to_js(ctx, ok);
}

JSValue js_component_get_child_count(JSContext* ctx, JSValueConst /*this_val*/, int argc,
                                     JSValueConst* argv) {
    if (argc < 1) {
        return throw_type_error(ctx, "engine.component.getChildCount(index) expects 1 argument");
    }
    int32_t index = -1;
    from_js(ctx, argv[0], index);
    Component* comp = component_at(ctx, index);
    if (!comp) return to_js(ctx, -1);
    int count = 0;
    for (Component* child : comp->component_children()) {
        if (component_index_of(ctx, child) < 0) continue;  // 内部组件不对外可见
        ++count;
    }
    return to_js(ctx, count);
}

JSValue js_component_get_child(JSContext* ctx, JSValueConst /*this_val*/, int argc,
                               JSValueConst* argv) {
    if (argc < 2) {
        return throw_type_error(ctx, "engine.component.getChild(index, childIndex) expects 2 arguments");
    }
    int32_t index = -1;
    int32_t child_index = -1;
    from_js(ctx, argv[0], index);
    from_js(ctx, argv[1], child_index);
    Component* comp = component_at(ctx, index);
    if (!comp || child_index < 0) return to_js(ctx, -1);
    int seen = 0;
    for (Component* child : comp->component_children()) {
        const int child_idx = component_index_of(ctx, child);
        if (child_idx < 0) continue;
        if (seen == child_index) return to_js(ctx, child_idx);
        ++seen;
    }
    return to_js(ctx, -1);
}

JSValue js_component_is_effectively_enabled(JSContext* ctx, JSValueConst /*this_val*/, int argc,
                                            JSValueConst* argv) {
    if (argc < 1) {
        return throw_type_error(ctx, "engine.component.isEffectivelyEnabled(index) expects 1 argument");
    }
    int32_t index = -1;
    from_js(ctx, argv[0], index);
    Component* comp = component_at(ctx, index);
    if (!comp) return to_js(ctx, false);
    return to_js(ctx, comp->is_effectively_enabled());
}

JSValue js_component_get_local_position(JSContext* ctx, JSValueConst /*this_val*/, int argc,
                                        JSValueConst* argv) {
    if (argc < 1) {
        return throw_type_error(ctx, "engine.component.getLocalPosition(index) expects 1 argument");
    }
    int32_t index = -1;
    from_js(ctx, argv[0], index);
    Component* comp = component_at(ctx, index);
    if (!comp) return JS_UNDEFINED;
    return to_js(ctx, comp->local_position());
}

JSValue js_component_set_local_position(JSContext* ctx, JSValueConst /*this_val*/, int argc,
                                        JSValueConst* argv) {
    if (argc < 2) {
        return throw_type_error(ctx, "engine.component.setLocalPosition(index, [x,y,z]) expects 2 arguments");
    }
    int32_t index = -1;
    from_js(ctx, argv[0], index);
    Component* comp = component_at(ctx, index);
    if (!comp) return to_js(ctx, false);
    math::Vector3f p;
    if (!from_js(ctx, argv[1], p)) {
        return throw_type_error(ctx, "engine.component.setLocalPosition(index, [x,y,z])");
    }
    comp->set_local_position(p);
    return to_js(ctx, true);
}

JSValue js_component_get_local_rotation(JSContext* ctx, JSValueConst /*this_val*/, int argc,
                                        JSValueConst* argv) {
    if (argc < 1) {
        return throw_type_error(ctx, "engine.component.getLocalRotation(index) expects 1 argument");
    }
    int32_t index = -1;
    from_js(ctx, argv[0], index);
    Component* comp = component_at(ctx, index);
    if (!comp) return JS_UNDEFINED;
    return to_js(ctx, comp->local_rotation());
}

JSValue js_component_set_local_rotation(JSContext* ctx, JSValueConst /*this_val*/, int argc,
                                        JSValueConst* argv) {
    if (argc < 2) {
        return throw_type_error(ctx, "engine.component.setLocalRotation(index, [x,y,z,w]) expects 2 arguments");
    }
    int32_t index = -1;
    from_js(ctx, argv[0], index);
    Component* comp = component_at(ctx, index);
    if (!comp) return to_js(ctx, false);
    math::Quaternionf q;
    if (!from_js(ctx, argv[1], q)) {
        return throw_type_error(ctx, "engine.component.setLocalRotation(index, [x,y,z,w])");
    }
    comp->set_local_rotation(q);
    return to_js(ctx, true);
}

JSValue js_component_get_local_scale(JSContext* ctx, JSValueConst /*this_val*/, int argc,
                                     JSValueConst* argv) {
    if (argc < 1) {
        return throw_type_error(ctx, "engine.component.getLocalScale(index) expects 1 argument");
    }
    int32_t index = -1;
    from_js(ctx, argv[0], index);
    Component* comp = component_at(ctx, index);
    if (!comp) return JS_UNDEFINED;
    return to_js(ctx, comp->local_scale());
}

JSValue js_component_set_local_scale(JSContext* ctx, JSValueConst /*this_val*/, int argc,
                                     JSValueConst* argv) {
    if (argc < 2) {
        return throw_type_error(ctx, "engine.component.setLocalScale(index, [x,y,z]) expects 2 arguments");
    }
    int32_t index = -1;
    from_js(ctx, argv[0], index);
    Component* comp = component_at(ctx, index);
    if (!comp) return to_js(ctx, false);
    math::Vector3f s;
    if (!from_js(ctx, argv[1], s)) {
        return throw_type_error(ctx, "engine.component.setLocalScale(index, [x,y,z])");
    }
    comp->set_local_scale(s);
    return to_js(ctx, true);
}

JSValue js_component_get_type_chain(JSContext* ctx, JSValueConst /*this_val*/, int argc,
                                     JSValueConst* argv) {
    if (argc < 1) {
        return throw_type_error(ctx, "engine.component.getTypeChain(index) expects 1 argument");
    }
    int32_t index = -1;
    from_js(ctx, argv[0], index);
    Component* comp = component_at(ctx, index);
    JSValue arr = JS_NewArray(ctx);
    if (!comp) return arr;
    // 祖先链（根 → 派生，含自身）；每项给短名，脚本侧按短名写类型即可。
    const auto names = gryce_engine::components::component_type_chain_names(comp);
    for (size_t i = 0; i < names.size(); ++i) {
        JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i), to_js(ctx, short_type_name(names[i])));
    }
    return arr;
}

JSValue js_component_is_derived_from(JSContext* ctx, JSValueConst /*this_val*/, int argc,
                                     JSValueConst* argv) {
    if (argc < 2) {
        return throw_type_error(ctx, "engine.component.isDerivedFrom(index, typeName) expects 2 arguments");
    }
    int32_t index = -1;
    from_js(ctx, argv[0], index);
    std::string base_name;
    if (!from_js(ctx, argv[1], base_name)) {
        return throw_type_error(ctx, "engine.component.isDerivedFrom(index, typeName)");
    }
    Component* comp = component_at(ctx, index);
    if (!comp) return to_js(ctx, false);
    const std::string base_short = short_type_name(base_name);
    for (const auto& name : gryce_engine::components::component_type_chain_names(comp)) {
        if (short_type_name(name) == base_short) return to_js(ctx, true);
    }
    return to_js(ctx, false);
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

    // engine.component.*：组件树（实例级父子挂载）与类型继承。
    // 组件下标与 C API 同口径（宿主实体上的可见组件序列）。
    JSValue component_obj = JS_NewObject(ctx);
    set_fn(ctx, component_obj, "count", &js_component_count, 0);
    set_fn(ctx, component_obj, "getParent", &js_component_get_parent, 1);
    set_fn(ctx, component_obj, "setParent", &js_component_set_parent, 2);
    set_fn(ctx, component_obj, "getChildCount", &js_component_get_child_count, 1);
    set_fn(ctx, component_obj, "getChild", &js_component_get_child, 2);
    set_fn(ctx, component_obj, "isEffectivelyEnabled", &js_component_is_effectively_enabled, 1);
    set_fn(ctx, component_obj, "getLocalPosition", &js_component_get_local_position, 1);
    set_fn(ctx, component_obj, "setLocalPosition", &js_component_set_local_position, 2);
    set_fn(ctx, component_obj, "getLocalRotation", &js_component_get_local_rotation, 1);
    set_fn(ctx, component_obj, "setLocalRotation", &js_component_set_local_rotation, 2);
    set_fn(ctx, component_obj, "getLocalScale", &js_component_get_local_scale, 1);
    set_fn(ctx, component_obj, "setLocalScale", &js_component_set_local_scale, 2);
    set_fn(ctx, component_obj, "getTypeChain", &js_component_get_type_chain, 1);
    set_fn(ctx, component_obj, "isDerivedFrom", &js_component_is_derived_from, 2);
    JS_SetPropertyStr(ctx, engine, "component", component_obj);

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
