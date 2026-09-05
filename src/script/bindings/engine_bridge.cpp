#include "engine_bridge.h"

#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <sstream>

#include <quickjs/quickjs-libc.h>

#include "GryceCore/core_api.h"
#include "GryceCore/types.h"
#include "GryceCore/entity_api.h"
#include "GryceCore/component_api.h"
#include "GryceCore/scene_api.h"
#include "GryceCore/script_api.h"

#include "script/runtime/script_context.h"
#include "utils/glog/glog_lib.h"

namespace GryceEngineUtils::script {

// ============================================================================
// 辅助函数
// ============================================================================

namespace {

// 从 JS 参数中提取字符串
const char* js_get_string(JSContext* ctx, JSValueConst v, const char* fallback = "") {
    if (JS_IsString(v)) {
        return JS_ToCString(ctx, v);
    }
    return fallback;
}

// JSValue 数组转 C 字符串数组（用于 component.get 返回值）
JSValue vector3_to_jsvalue(JSContext* ctx, float x, float y, float z) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "x", JS_NewFloat64(ctx, x));
    JS_SetPropertyStr(ctx, obj, "y", JS_NewFloat64(ctx, y));
    JS_SetPropertyStr(ctx, obj, "z", JS_NewFloat64(ctx, z));
    return obj;
}

JSValue vector2_to_jsvalue(JSContext* ctx, float x, float y) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "x", JS_NewFloat64(ctx, x));
    JS_SetPropertyStr(ctx, obj, "y", JS_NewFloat64(ctx, y));
    return obj;
}

// 获取组件类型 hash 通过名称
uint64_t get_component_type_hash(const char* type_name) {
    int count = GComponent_GetRegisteredTypeCount();
    for (int i = 0; i < count; ++i) {
        uint64_t hash = 0;
        char name[128];
        if (GComponent_GetRegisteredTypeInfo(i, &hash, name, sizeof(name)) == 0) {
            if (std::strcmp(name, type_name) == 0) {
                return hash;
            }
        }
    }
    return 0;
}

} // namespace

// ============================================================================
// engine.log
// ============================================================================

static JSValue js_engine_log_info(JSContext* ctx, JSValueConst this_val,
                                   int argc, JSValueConst* argv) {
    std::string msg;
    for (int i = 0; i < argc; ++i) {
        if (i > 0) msg += " ";
        const char* str = JS_ToCString(ctx, argv[i]);
        if (str) {
            msg += str;
            JS_FreeCString(ctx, str);
        }
    }
    GLOG_INFO("[JS] {}", msg);
    return JS_UNDEFINED;
}

static JSValue js_engine_log_warn(JSContext* ctx, JSValueConst this_val,
                                   int argc, JSValueConst* argv) {
    std::string msg;
    for (int i = 0; i < argc; ++i) {
        if (i > 0) msg += " ";
        const char* str = JS_ToCString(ctx, argv[i]);
        if (str) {
            msg += str;
            JS_FreeCString(ctx, str);
        }
    }
    GLOG_WARN("[JS] {}", msg);
    return JS_UNDEFINED;
}

static JSValue js_engine_log_error(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv) {
    std::string msg;
    for (int i = 0; i < argc; ++i) {
        if (i > 0) msg += " ";
        const char* str = JS_ToCString(ctx, argv[i]);
        if (str) {
            msg += str;
            JS_FreeCString(ctx, str);
        }
    }
    GLOG_ERROR("[JS] {}", msg);
    return JS_UNDEFINED;
}

// ============================================================================
// engine.version()
// ============================================================================

static JSValue js_engine_version(JSContext* ctx, JSValueConst this_val,
                                  int argc, JSValueConst* argv) {
    return JS_NewString(ctx, "GryceEngine 0.1.0 (QuickJS)");
}

// ============================================================================
// engine.self()
// ============================================================================

static JSValue js_engine_self(JSContext* ctx, JSValueConst this_val,
                               int argc, JSValueConst* argv) {
    int handle = ScriptContext::instance().current_entity();
    return JS_NewInt32(ctx, handle);
}

// ============================================================================
// engine.entity.*
// ============================================================================

// engine.entity.get_name(handle) -> string
static JSValue js_entity_get_name(JSContext* ctx, JSValueConst this_val,
                                   int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "get_name: handle required");
    int32_t handle;
    if (JS_ToInt32(ctx, &handle, argv[0])) return JS_EXCEPTION;

    char buf[256];
    if (GEntity_GetName(handle, buf, sizeof(buf)) == 0) {
        return JS_NewString(ctx, buf);
    }
    return JS_NewString(ctx, "");
}

// engine.entity.find(name) -> handle | 0
static JSValue js_entity_find(JSContext* ctx, JSValueConst this_val,
                               int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "find: name required");
    const char* name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_ThrowTypeError(ctx, "find: name must be a string");

    int count = GEntity_GetCount();
    for (int i = 0; i < count; ++i) {
        GEntityHandle h = GEntity_GetAt(i);
        if (h == 0) continue;
        char buf[256];
        if (GEntity_GetName(h, buf, sizeof(buf)) == 0 && std::strcmp(buf, name) == 0) {
            JS_FreeCString(ctx, name);
            return JS_NewInt32(ctx, h);
        }
    }
    JS_FreeCString(ctx, name);
    return JS_NewInt32(ctx, 0);
}

// engine.entity.find_all(name) -> array of handles
static JSValue js_entity_find_all(JSContext* ctx, JSValueConst this_val,
                                   int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "find_all: name required");
    const char* name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_ThrowTypeError(ctx, "find_all: name must be a string");

    JSValue arr = JS_NewArray(ctx);
    int arr_idx = 0;
    int count = GEntity_GetCount();
    for (int i = 0; i < count; ++i) {
        GEntityHandle h = GEntity_GetAt(i);
        if (h == 0) continue;
        char buf[256];
        if (GEntity_GetName(h, buf, sizeof(buf)) == 0 && std::strcmp(buf, name) == 0) {
            JS_SetPropertyUint32(ctx, arr, arr_idx++, JS_NewInt32(ctx, h));
        }
    }
    JS_FreeCString(ctx, name);
    return arr;
}

// engine.entity.create(name, parent) -> handle
static JSValue js_entity_create(JSContext* ctx, JSValueConst this_val,
                                 int argc, JSValueConst* argv) {
    const char* name = "NewEntity";
    if (argc >= 1) {
        const char* s = JS_ToCString(ctx, argv[0]);
        if (s) { name = s; }
    }

    GEntityHandle parent = 0;
    if (argc >= 2) {
        JS_ToInt32(ctx, &parent, argv[1]);
    }

    struct Payload { char name[128]; GEntityHandle parent; };
    Payload p;
    std::memset(&p, 0, sizeof(p));
    std::strncpy(p.name, name, sizeof(p.name) - 1);
    p.parent = parent;

    GCommand cmd;
    std::memset(&cmd, 0, sizeof(cmd));
    cmd.type = ECMD_CREATE_ENTITY;
    std::memcpy(cmd.payload, &p, sizeof(p));
    GCore_PushCommand(&cmd);

    if (argc >= 1) JS_FreeCString(ctx, name);
    return JS_NewInt32(ctx, 0);
}

// engine.entity.destroy(handle)
static JSValue js_entity_destroy(JSContext* ctx, JSValueConst this_val,
                                  int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "destroy: handle required");
    int32_t handle;
    if (JS_ToInt32(ctx, &handle, argv[0])) return JS_EXCEPTION;

    GCommand cmd;
    std::memset(&cmd, 0, sizeof(cmd));
    cmd.type = ECMD_DESTROY_ENTITY;
    std::memcpy(cmd.payload, &handle, sizeof(handle));
    GCore_PushCommand(&cmd);

    return JS_UNDEFINED;
}

// engine.entity.aabb(handle) -> {x, y, z, w, h, d} | null
static JSValue js_entity_aabb(JSContext* ctx, JSValueConst this_val,
                               int argc, JSValueConst* argv) {
    // No C API for AABB, return stub
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "x", JS_NewFloat64(ctx, 0));
    JS_SetPropertyStr(ctx, obj, "y", JS_NewFloat64(ctx, 0));
    JS_SetPropertyStr(ctx, obj, "z", JS_NewFloat64(ctx, 0));
    JS_SetPropertyStr(ctx, obj, "w", JS_NewFloat64(ctx, 1));
    JS_SetPropertyStr(ctx, obj, "h", JS_NewFloat64(ctx, 1));
    JS_SetPropertyStr(ctx, obj, "d", JS_NewFloat64(ctx, 1));
    return obj;
}

// engine.entity.get_transform(handle) -> {position, rotation, scale}
static JSValue js_entity_get_transform(JSContext* ctx, JSValueConst this_val,
                                        int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "get_transform: handle required");
    int32_t handle;
    if (JS_ToInt32(ctx, &handle, argv[0])) return JS_EXCEPTION;

    GVec3 pos, scale;
    GQuat rot;
    GEntity_GetLocalPosition(handle, &pos);
    GEntity_GetLocalRotation(handle, &rot);
    GEntity_GetLocalScale(handle, &scale);

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "position", vector3_to_jsvalue(ctx, pos.x, pos.y, pos.z));
    JS_SetPropertyStr(ctx, obj, "rotation", vector3_to_jsvalue(ctx, rot.x, rot.y, rot.z));
    JS_SetPropertyStr(ctx, obj, "scale", vector3_to_jsvalue(ctx, scale.x, scale.y, scale.z));
    return obj;
}

// engine.entity.set_transform(handle, transform)
static JSValue js_entity_set_transform(JSContext* ctx, JSValueConst this_val,
                                        int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "set_transform: handle and transform required");
    int32_t handle;
    if (JS_ToInt32(ctx, &handle, argv[0])) return JS_EXCEPTION;

    JSValue trans = argv[1];

    // Set position
    JSValue pos = JS_GetPropertyStr(ctx, trans, "position");
    if (!JS_IsUndefined(pos)) {
        double x, y, z;
        JSValue vx = JS_GetPropertyStr(ctx, pos, "x");
        JSValue vy = JS_GetPropertyStr(ctx, pos, "y");
        JSValue vz = JS_GetPropertyStr(ctx, pos, "z");
        JS_ToFloat64(ctx, &x, vx);
        JS_ToFloat64(ctx, &y, vy);
        JS_ToFloat64(ctx, &z, vz);
        GVec3 p = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
        GEntity_SetLocalPosition(handle, &p);
        JS_FreeValue(ctx, vx); JS_FreeValue(ctx, vy); JS_FreeValue(ctx, vz);
        JS_FreeValue(ctx, pos);
    }

    // Set rotation (quaternion)
    JSValue rot = JS_GetPropertyStr(ctx, trans, "rotation");
    if (!JS_IsUndefined(rot)) {
        double x, y, z, w;
        JSValue vx = JS_GetPropertyStr(ctx, rot, "x");
        JSValue vy = JS_GetPropertyStr(ctx, rot, "y");
        JSValue vz = JS_GetPropertyStr(ctx, rot, "z");
        JS_ToFloat64(ctx, &x, vx);
        JS_ToFloat64(ctx, &y, vy);
        JS_ToFloat64(ctx, &z, vz);
        JS_FreeValue(ctx, vx); JS_FreeValue(ctx, vy); JS_FreeValue(ctx, vz);
        // Check if w is provided
        JSValue vw = JS_GetPropertyStr(ctx, rot, "w");
        if (!JS_IsUndefined(vw)) {
            JS_ToFloat64(ctx, &w, vw);
            GQuat r = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z), static_cast<float>(w)};
            GEntity_SetLocalRotation(handle, &r);
            JS_FreeValue(ctx, vw);
        }
        JS_FreeValue(ctx, rot);
    }

    // Set scale
    JSValue sc = JS_GetPropertyStr(ctx, trans, "scale");
    if (!JS_IsUndefined(sc)) {
        double x, y, z;
        JSValue vx = JS_GetPropertyStr(ctx, sc, "x");
        JSValue vy = JS_GetPropertyStr(ctx, sc, "y");
        JSValue vz = JS_GetPropertyStr(ctx, sc, "z");
        JS_ToFloat64(ctx, &x, vx);
        JS_ToFloat64(ctx, &y, vy);
        JS_ToFloat64(ctx, &z, vz);
        GVec3 s = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
        GEntity_SetLocalScale(handle, &s);
        JS_FreeValue(ctx, vx); JS_FreeValue(ctx, vy); JS_FreeValue(ctx, vz);
        JS_FreeValue(ctx, sc);
    }

    return JS_UNDEFINED;
}

// ============================================================================
// engine.component.*
// ============================================================================

// engine.component.has(handle?, type) -> bool
// handle 可省略，省略时使用当前实体（engine.self()）
static JSValue js_component_has(JSContext* ctx, JSValueConst this_val,
                                 int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "has: type required");
    int32_t handle;
    int type_idx = 0;
    if (argc >= 2) {
        if (JS_ToInt32(ctx, &handle, argv[0])) return JS_EXCEPTION;
        type_idx = 1;
    } else {
        handle = ScriptContext::instance().current_entity();
        type_idx = 0;
    }
    const char* type_name = JS_ToCString(ctx, argv[type_idx]);
    if (!type_name) return JS_ThrowTypeError(ctx, "has: type must be a string");

    uint64_t hash = get_component_type_hash(type_name);
    JS_FreeCString(ctx, type_name);

    if (hash == 0) return JS_FALSE;

    // 检查实体是否包含该组件
    int count = GComponent_GetCount(handle);
    for (int i = 0; i < count; ++i) {
        uint64_t h = 0;
        char name[128];
        if (GComponent_GetTypeHashAt(handle, i, &h) == 0 && h == hash) {
            return JS_TRUE;
        }
    }
    return JS_FALSE;
}

// engine.component.get(handle?, type) -> {prop: value} | null
// handle 可省略，省略时使用当前实体（engine.self()）
static JSValue js_component_get(JSContext* ctx, JSValueConst this_val,
                                 int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "get: type required");
    int32_t handle;
    int type_idx = 0;
    if (argc >= 2) {
        if (JS_ToInt32(ctx, &handle, argv[0])) return JS_EXCEPTION;
        type_idx = 1;
    } else {
        handle = ScriptContext::instance().current_entity();
        type_idx = 0;
    }
    const char* type_name = JS_ToCString(ctx, argv[type_idx]);
    if (!type_name) return JS_ThrowTypeError(ctx, "get: type must be a string");

    uint64_t hash = get_component_type_hash(type_name);
    if (hash == 0) {
        JS_FreeCString(ctx, type_name);
        return JS_NULL;
    }

    // 读取所有属性
    int prop_count = GComponent_GetPropertyCount(handle, hash);
    JSValue obj = JS_NewObject(ctx);

    for (int i = 0; i < prop_count; ++i) {
        char name[128];
        int prop_type = 0, prop_size = 0;
        if (GComponent_GetPropertyInfo(handle, hash, i, name, sizeof(name), &prop_type, &prop_size) != 0) {
            continue;
        }

        // 根据类型读取值
        switch (prop_type) {
            case 0: { // float
                float val = 0;
                if (GComponent_GetProperty(handle, hash, name, &val, sizeof(val)) == 0) {
                    JS_SetPropertyStr(ctx, obj, name, JS_NewFloat64(ctx, val));
                }
                break;
            }
            case 1: { // int
                int val = 0;
                if (GComponent_GetProperty(handle, hash, name, &val, sizeof(val)) == 0) {
                    JS_SetPropertyStr(ctx, obj, name, JS_NewInt32(ctx, val));
                }
                break;
            }
            case 2: { // bool
                int val = 0;
                if (GComponent_GetProperty(handle, hash, name, &val, sizeof(val)) == 0) {
                    JS_SetPropertyStr(ctx, obj, name, JS_NewBool(ctx, val != 0));
                }
                break;
            }
            case 3: { // string
                char buf[256] = {0};
                if (GComponent_GetProperty(handle, hash, name, buf, sizeof(buf)) == 0) {
                    JS_SetPropertyStr(ctx, obj, name, JS_NewString(ctx, buf));
                }
                break;
            }
            default: {
                // 未知类型，尝试读取 4 字节
                int32_t val = 0;
                if (GComponent_GetProperty(handle, hash, name, &val, sizeof(val)) == 0) {
                    JS_SetPropertyStr(ctx, obj, name, JS_NewInt32(ctx, val));
                }
                break;
            }
        }
    }

    JS_FreeCString(ctx, type_name);
    return obj;
}

// engine.component.set(handle?, type, props) -> success
// handle 可省略，省略时使用当前实体（engine.self()）
static JSValue js_component_set(JSContext* ctx, JSValueConst this_val,
                                 int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "set: type and props required");
    int32_t handle;
    int type_idx = 0, props_idx = 1;
    if (argc >= 3) {
        if (JS_ToInt32(ctx, &handle, argv[0])) return JS_EXCEPTION;
        type_idx = 1;
        props_idx = 2;
    } else {
        handle = ScriptContext::instance().current_entity();
        type_idx = 0;
        props_idx = 1;
    }
    const char* type_name = JS_ToCString(ctx, argv[type_idx]);
    if (!type_name) return JS_ThrowTypeError(ctx, "set: type must be a string");

    JSValue props = argv[props_idx];
    if (!JS_IsObject(props)) {
        JS_FreeCString(ctx, type_name);
        return JS_ThrowTypeError(ctx, "set: props must be an object");
    }

    uint64_t hash = get_component_type_hash(type_name);
    if (hash == 0) {
        JS_FreeCString(ctx, type_name);
        return JS_FALSE;
    }

    // 获取所有属性名并设置
    JSPropertyEnum* props_tab = nullptr;
    uint32_t props_len = 0;
    int ret = JS_GetOwnPropertyNames(ctx, &props_tab, &props_len, props,
                                      JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY);
    if (ret == 0 && props_tab) {
        for (uint32_t i = 0; i < props_len; ++i) {
            const char* prop_name = JS_AtomToCString(ctx, props_tab[i].atom);
            if (!prop_name) continue;

            JSValue val = JS_GetProperty(ctx, props, props_tab[i].atom);

            // 尝试读取属性信息以确定类型
            int prop_count = GComponent_GetPropertyCount(handle, hash);
            bool found = false;
            for (int p = 0; p < prop_count; ++p) {
                char pname[128];
                int ptype = 0, psize = 0;
                if (GComponent_GetPropertyInfo(handle, hash, p, pname, sizeof(pname), &ptype, &psize) != 0) continue;
                if (std::strcmp(pname, prop_name) != 0) continue;

                found = true;
                switch (ptype) {
                    case 0: { // float
                        double d;
                        JS_ToFloat64(ctx, &d, val);
                        float f = static_cast<float>(d);
                        GComponent_SetProperty(handle, hash, prop_name, &f, sizeof(f));
                        break;
                    }
                    case 1: { // int
                        int32_t iv;
                        JS_ToInt32(ctx, &iv, val);
                        GComponent_SetProperty(handle, hash, prop_name, &iv, sizeof(iv));
                        break;
                    }
                    case 2: { // bool
                        int b = JS_ToBool(ctx, val);
                        GComponent_SetProperty(handle, hash, prop_name, &b, sizeof(b));
                        break;
                    }
                    case 3: { // string
                        const char* s = JS_ToCString(ctx, val);
                        if (s) {
                            GComponent_SetProperty(handle, hash, prop_name, s, static_cast<int>(std::strlen(s) + 1));
                            JS_FreeCString(ctx, s);
                        }
                        break;
                    }
                    default: {
                        double d;
                        JS_ToFloat64(ctx, &d, val);
                        float f = static_cast<float>(d);
                        GComponent_SetProperty(handle, hash, prop_name, &f, sizeof(f));
                        break;
                    }
                }
                break;
            }

            if (!found) {
                // 属性不存在，尝试以 float 写入
                double d;
                if (JS_ToFloat64(ctx, &d, val) == 0) {
                    float f = static_cast<float>(d);
                    GComponent_SetProperty(handle, hash, prop_name, &f, sizeof(f));
                }
            }

            JS_FreeValue(ctx, val);
            JS_FreeCString(ctx, prop_name);
        }
        JS_FreePropertyEnum(ctx, props_tab, props_len);
    }

    JS_FreeCString(ctx, type_name);
    return JS_TRUE;
}

// ============================================================================
// engine.state.*
// ============================================================================

static JSValue js_state_get(JSContext* ctx, JSValueConst this_val,
                             int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "get: key required");
    const char* key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_ThrowTypeError(ctx, "get: key must be a string");

    std::string val = ScriptContext::instance().get_state(key);
    JS_FreeCString(ctx, key);
    return JS_NewString(ctx, val.c_str());
}

static JSValue js_state_set(JSContext* ctx, JSValueConst this_val,
                             int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "set: key and value required");
    const char* key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_ThrowTypeError(ctx, "set: key must be a string");

    // 将值转为字符串
    std::string val;
    if (JS_IsString(argv[1])) {
        const char* s = JS_ToCString(ctx, argv[1]);
        if (s) { val = s; JS_FreeCString(ctx, s); }
    } else if (JS_IsNumber(argv[1])) {
        double d;
        JS_ToFloat64(ctx, &d, argv[1]);
        // 整数值格式化为整数形式（如 250 而不是 250.000000）
        if (std::floor(d) == d &&
            d >= -9007199254740992.0 && d <= 9007199254740992.0) {
            val = std::to_string(static_cast<long long>(d));
        } else {
            val = std::to_string(d);
        }
    } else if (JS_IsBool(argv[1])) {
        val = JS_ToBool(ctx, argv[1]) ? "true" : "false";
    }

    ScriptContext::instance().set_state(key, val);
    JS_FreeCString(ctx, key);
    return JS_UNDEFINED;
}

static JSValue js_state_has(JSContext* ctx, JSValueConst this_val,
                             int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "has: key required");
    const char* key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_ThrowTypeError(ctx, "has: key must be a string");

    bool has = ScriptContext::instance().has_state(key);
    JS_FreeCString(ctx, key);
    return JS_NewBool(ctx, has);
}

// ============================================================================
// engine.input.*
// ============================================================================

static JSValue js_input_key_down(JSContext* ctx, JSValueConst this_val,
                                  int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "key_down: key code required");
    int32_t key;
    if (JS_ToInt32(ctx, &key, argv[0])) return JS_EXCEPTION;
    return JS_NewBool(ctx, ScriptContext::instance().is_key_down(key));
}

static JSValue js_input_mouse_pos(JSContext* ctx, JSValueConst this_val,
                                   int argc, JSValueConst* argv) {
    float x, y;
    ScriptContext::instance().get_mouse_pos(&x, &y);
    return vector2_to_jsvalue(ctx, x, y);
}

static JSValue js_input_mouse_down(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "mouse_down: button required");
    int32_t btn;
    if (JS_ToInt32(ctx, &btn, argv[0])) return JS_EXCEPTION;
    return JS_NewBool(ctx, ScriptContext::instance().is_mouse_down(btn));
}

// ============================================================================
// engine.time.*
// ============================================================================

static JSValue js_time_delta(JSContext* ctx, JSValueConst this_val,
                              int argc, JSValueConst* argv) {
    return JS_NewFloat64(ctx, ScriptContext::instance().delta_time());
}

static JSValue js_time_elapsed(JSContext* ctx, JSValueConst this_val,
                                int argc, JSValueConst* argv) {
    return JS_NewFloat64(ctx, ScriptContext::instance().elapsed_time());
}

// ============================================================================
// engine.scene.*
// ============================================================================

static JSValue js_scene_load(JSContext* ctx, JSValueConst this_val,
                              int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "load: path required");
    const char* path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_ThrowTypeError(ctx, "load: path must be a string");

    GScene_Load(path);
    GLOG_INFO("[JS] engine.scene.load(\"{}\")", path);
    JS_FreeCString(ctx, path);
    return JS_UNDEFINED;
}

static JSValue js_scene_current(JSContext* ctx, JSValueConst this_val,
                                 int argc, JSValueConst* argv) {
    char buf[256] = {0};
    if (GScene_GetCurrentPath(buf, sizeof(buf)) == 0) {
        return JS_NewString(ctx, buf);
    }
    return JS_NewString(ctx, "");
}

// ============================================================================
// engine.audio.play_on(handle)
// ============================================================================

static JSValue js_audio_play_on(JSContext* ctx, JSValueConst this_val,
                                 int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "play_on: handle required");
    GLOG_WARN("[JS] engine.audio.play_on not fully implemented yet");
    return JS_UNDEFINED;
}

// ============================================================================
// engine.fx.burst(handle)
// ============================================================================

static JSValue js_fx_burst(JSContext* ctx, JSValueConst this_val,
                            int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "burst: handle required");
    GLOG_WARN("[JS] engine.fx.burst not fully implemented yet");
    return JS_UNDEFINED;
}

// ============================================================================
// engine.json.read(path)
// ============================================================================

static JSValue js_json_read(JSContext* ctx, JSValueConst this_val,
                             int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "read: path required");
    const char* path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_ThrowTypeError(ctx, "read: path must be a string");

    GLOG_WARN("[JS] engine.json.read(\"{}\") not fully implemented yet", path);
    JS_FreeCString(ctx, path);
    return JS_NewString(ctx, "{}");
}

// ============================================================================
// engine.physics.*
// ============================================================================

static JSValue js_physics_set_gravity(JSContext* ctx, JSValueConst this_val,
                                       int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "set_gravity: x and y required");
    double x, y;
    if (JS_ToFloat64(ctx, &x, argv[0])) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &y, argv[1])) return JS_EXCEPTION;

    GCommand cmd;
    std::memset(&cmd, 0, sizeof(cmd));
    cmd.type = ECMD_PHYSICS_SET_GRAVITY;
    float g[2] = {static_cast<float>(x), static_cast<float>(y)};
    std::memcpy(cmd.payload, g, sizeof(g));
    GCore_PushCommand(&cmd);

    return JS_UNDEFINED;
}

static JSValue js_physics_get_gravity(JSContext* ctx, JSValueConst this_val,
                                       int argc, JSValueConst* argv) {
    // No direct C API, return default
    return vector2_to_jsvalue(ctx, 0, -9.8f);
}

// ============================================================================
// engine.loadScene (UI 兼容别名)
// ============================================================================

static JSValue js_engine_loadScene(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "loadScene: path required");
    const char* path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_ThrowTypeError(ctx, "loadScene: path must be a string");

    GCommand cmd;
    std::memset(&cmd, 0, sizeof(cmd));
    cmd.type = ECMD_LOAD_SCENE;
    std::strncpy(reinterpret_cast<char*>(cmd.payload), path, GCMD_PAYLOAD_SIZE - 1);
    GCore_PushCommand(&cmd);

    GLOG_INFO("[JS] engine.loadScene(\"{}\")", path);
    JS_FreeCString(ctx, path);
    return JS_UNDEFINED;
}

// ============================================================================
// engine.showDialog / closeDialog / bind (UI)
// ============================================================================

static JSValue js_engine_showDialog(JSContext* ctx, JSValueConst this_val,
                                     int argc, JSValueConst* argv) {
    // UI 功能：通过 UIManager 操作
    GLOG_WARN("[JS] engine.showDialog: UI not available in pure script context");
    return JS_UNDEFINED;
}

static JSValue js_engine_closeDialog(JSContext* ctx, JSValueConst this_val,
                                      int argc, JSValueConst* argv) {
    GLOG_WARN("[JS] engine.closeDialog: UI not available in pure script context");
    return JS_UNDEFINED;
}

static JSValue js_engine_bind(JSContext* ctx, JSValueConst this_val,
                               int argc, JSValueConst* argv) {
    GLOG_WARN("[JS] engine.bind: UI not available in pure script context");
    return JS_UNDEFINED;
}

// ============================================================================
// engine.playSound (UI 兼容)
// ============================================================================

static JSValue js_engine_playSound(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv) {
    GLOG_WARN("[JS] engine.playSound: UI not available in pure script context");
    return JS_UNDEFINED;
}

// ============================================================================
// engine.getString (UI 兼容)
// ============================================================================

static JSValue js_engine_getString(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "getString: key argument required");
    const char* key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_ThrowTypeError(ctx, "getString: key must be a string");
    JSValue ret = JS_NewString(ctx, key);
    JS_FreeCString(ctx, key);
    return ret;
}

// ============================================================================
// 注册入口
// ============================================================================

void register_engine_bindings(JSContext* ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue engine = JS_NewObject(ctx);

    // ---- engine.log ----
    JSValue log_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, log_obj, "info", JS_NewCFunction(ctx, js_engine_log_info, "info", 1));
    JS_SetPropertyStr(ctx, log_obj, "warn", JS_NewCFunction(ctx, js_engine_log_warn, "warn", 1));
    JS_SetPropertyStr(ctx, log_obj, "error", JS_NewCFunction(ctx, js_engine_log_error, "error", 1));
    JS_SetPropertyStr(ctx, engine, "log", log_obj);

    // ---- engine.version() ----
    JS_SetPropertyStr(ctx, engine, "version", JS_NewCFunction(ctx, js_engine_version, "version", 0));

    // ---- engine.self() ----
    JS_SetPropertyStr(ctx, engine, "self", JS_NewCFunction(ctx, js_engine_self, "self", 0));

    // ---- engine.entity.* ----
    JSValue entity_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, entity_obj, "get_name", JS_NewCFunction(ctx, js_entity_get_name, "get_name", 1));
    JS_SetPropertyStr(ctx, entity_obj, "find", JS_NewCFunction(ctx, js_entity_find, "find", 1));
    JS_SetPropertyStr(ctx, entity_obj, "find_all", JS_NewCFunction(ctx, js_entity_find_all, "find_all", 1));
    JS_SetPropertyStr(ctx, entity_obj, "create", JS_NewCFunction(ctx, js_entity_create, "create", 2));
    JS_SetPropertyStr(ctx, entity_obj, "destroy", JS_NewCFunction(ctx, js_entity_destroy, "destroy", 1));
    JS_SetPropertyStr(ctx, entity_obj, "aabb", JS_NewCFunction(ctx, js_entity_aabb, "aabb", 1));
    JS_SetPropertyStr(ctx, entity_obj, "get_transform", JS_NewCFunction(ctx, js_entity_get_transform, "get_transform", 1));
    JS_SetPropertyStr(ctx, entity_obj, "set_transform", JS_NewCFunction(ctx, js_entity_set_transform, "set_transform", 2));
    JS_SetPropertyStr(ctx, engine, "entity", entity_obj);

    // ---- engine.component.* ----
    JSValue comp_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, comp_obj, "has", JS_NewCFunction(ctx, js_component_has, "has", 2));
    JS_SetPropertyStr(ctx, comp_obj, "get", JS_NewCFunction(ctx, js_component_get, "get", 2));
    JS_SetPropertyStr(ctx, comp_obj, "set", JS_NewCFunction(ctx, js_component_set, "set", 3));
    JS_SetPropertyStr(ctx, engine, "component", comp_obj);

    // ---- engine.state.* ----
    JSValue state_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, state_obj, "get", JS_NewCFunction(ctx, js_state_get, "get", 1));
    JS_SetPropertyStr(ctx, state_obj, "set", JS_NewCFunction(ctx, js_state_set, "set", 2));
    JS_SetPropertyStr(ctx, state_obj, "has", JS_NewCFunction(ctx, js_state_has, "has", 1));
    JS_SetPropertyStr(ctx, engine, "state", state_obj);

    // ---- engine.input.* ----
    JSValue input_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, input_obj, "key_down", JS_NewCFunction(ctx, js_input_key_down, "key_down", 1));
    JS_SetPropertyStr(ctx, input_obj, "mouse_pos", JS_NewCFunction(ctx, js_input_mouse_pos, "mouse_pos", 0));
    JS_SetPropertyStr(ctx, input_obj, "mouse_down", JS_NewCFunction(ctx, js_input_mouse_down, "mouse_down", 1));
    JS_SetPropertyStr(ctx, engine, "input", input_obj);

    // ---- engine.time.* ----
    JSValue time_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, time_obj, "delta", JS_NewCFunction(ctx, js_time_delta, "delta", 0));
    JS_SetPropertyStr(ctx, time_obj, "elapsed", JS_NewCFunction(ctx, js_time_elapsed, "elapsed", 0));
    JS_SetPropertyStr(ctx, engine, "time", time_obj);

    // ---- engine.scene.* ----
    JSValue scene_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, scene_obj, "load", JS_NewCFunction(ctx, js_scene_load, "load", 1));
    JS_SetPropertyStr(ctx, scene_obj, "current", JS_NewCFunction(ctx, js_scene_current, "current", 0));
    JS_SetPropertyStr(ctx, engine, "scene", scene_obj);

    // ---- engine.audio ----
    JSValue audio_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, audio_obj, "play_on", JS_NewCFunction(ctx, js_audio_play_on, "play_on", 1));
    JS_SetPropertyStr(ctx, engine, "audio", audio_obj);

    // ---- engine.fx ----
    JSValue fx_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, fx_obj, "burst", JS_NewCFunction(ctx, js_fx_burst, "burst", 1));
    JS_SetPropertyStr(ctx, engine, "fx", fx_obj);

    // ---- engine.json ----
    JSValue json_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, json_obj, "read", JS_NewCFunction(ctx, js_json_read, "read", 1));
    JS_SetPropertyStr(ctx, engine, "json", json_obj);

    // ---- engine.physics.* ----
    JSValue physics_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, physics_obj, "set_gravity", JS_NewCFunction(ctx, js_physics_set_gravity, "set_gravity", 2));
    JS_SetPropertyStr(ctx, physics_obj, "get_gravity", JS_NewCFunction(ctx, js_physics_get_gravity, "get_gravity", 0));
    JS_SetPropertyStr(ctx, engine, "physics", physics_obj);

    // ---- UI 兼容函数（直接挂在 engine 上） ----
	    JS_SetPropertyStr(ctx, engine, "loadScene", JS_NewCFunction(ctx, js_engine_loadScene, "loadScene", 1));
	    JS_SetPropertyStr(ctx, engine, "playSound", JS_NewCFunction(ctx, js_engine_playSound, "playSound", 1));
	    JS_SetPropertyStr(ctx, engine, "showDialog", JS_NewCFunction(ctx, js_engine_showDialog, "showDialog", 1));
	    JS_SetPropertyStr(ctx, engine, "closeDialog", JS_NewCFunction(ctx, js_engine_closeDialog, "closeDialog", 0));
	    JS_SetPropertyStr(ctx, engine, "getString", JS_NewCFunction(ctx, js_engine_getString, "getString", 1));
	    JS_SetPropertyStr(ctx, engine, "bind", JS_NewCFunction(ctx, js_engine_bind, "bind", 2));

    // 注册到全局
    JS_SetPropertyStr(ctx, global, "engine", engine);
    JS_FreeValue(ctx, global);

    GLOG_INFO("ScriptEngine: registered engine.* bindings (30+ functions)");
}

} // namespace GryceEngineUtils::script