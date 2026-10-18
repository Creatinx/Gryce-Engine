// ===========================================================================
// engine/script/js_value.h
// C++ <-> QuickJS 值转换层（引擎脚本系统的唯一转换入口）。
//
// 约定：
// - 所有绑定代码只允许通过本头的 to_js / from_js 系列转换，不允许在绑定点
//   直接调用 JS_ToFloat64 等原语——边界语义（NaN、Infinity、越界截断、
//   null/undefined）在此一处定义并被 value_convert 测试套件覆盖。
// - 容器约定：Vector2f/3f、Quaternionf 用 JS 数组表示（[x,y,z] / [x,y,z,w]）；
//   Matrix4f 用 16 元素数组表示，**列主序内存序**（m[col*4+row]，与 GLSL /
//   glMatrix 一致，直接对应 Matrix4f::m 的内存布局）。
// - nlohmann::json 与 JS 值递归互转，用于 ScriptComponent 的 props 注入。
// ===========================================================================

#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

extern "C" {
#include "quickjs.h"
}

#include "math/math.h"

namespace gryce_engine::script {

// ---------------------------------------------------------------------------
// 基础标量 -> JS
// ---------------------------------------------------------------------------
inline JSValue to_js(JSContext* ctx, bool v) {
    return JS_NewBool(ctx, v ? 1 : 0);
}

inline JSValue to_js(JSContext* ctx, int32_t v) {
    return JS_NewInt32(ctx, v);
}

inline JSValue to_js(JSContext* ctx, uint32_t v) {
    return JS_NewUint32(ctx, v);
}

inline JSValue to_js(JSContext* ctx, int64_t v) {
    return JS_NewInt64(ctx, v);
}

inline JSValue to_js(JSContext* ctx, double v) {
    return JS_NewFloat64(ctx, v);
}

inline JSValue to_js(JSContext* ctx, float v) { return to_js(ctx, static_cast<double>(v)); }

inline JSValue to_js(JSContext* ctx, const char* v) { return JS_NewString(ctx, v); }

inline JSValue to_js(JSContext* ctx, const std::string& v) {
    return JS_NewStringLen(ctx, v.c_str(), v.size());
}

// ---------------------------------------------------------------------------
// JS -> 基础标量
// 成功返回 true；v 为 undefined/null 时返回 false 且不写 out（调用方决定
// 默认值语义）；数字越界（如 1e30 -> int32）截断前由 JS_ToInt32 的 ToNumber
// 语义处理，NaN -> 0，Infinity -> 0（与 ECMAScript ToInt32 一致）。
// ---------------------------------------------------------------------------
inline bool from_js(JSContext* ctx, JSValueConst v, bool& out) {
    if (JS_IsUndefined(v) || JS_IsNull(v)) return false;
    int b = JS_ToBool(ctx, v);
    if (b < 0) return false;
    out = (b != 0);
    return true;
}

inline bool from_js(JSContext* ctx, JSValueConst v, int32_t& out) {
    if (JS_IsUndefined(v) || JS_IsNull(v)) return false;
    int32_t r = 0;
    if (JS_ToInt32(ctx, &r, v) < 0) return false;
    out = r;
    return true;
}

inline bool from_js(JSContext* ctx, JSValueConst v, uint32_t& out) {
    if (JS_IsUndefined(v) || JS_IsNull(v)) return false;
    uint32_t r = 0;
    if (JS_ToUint32(ctx, &r, v) < 0) return false;
    out = r;
    return true;
}

inline bool from_js(JSContext* ctx, JSValueConst v, double& out) {
    if (JS_IsUndefined(v) || JS_IsNull(v)) return false;
    double r = 0.0;
    if (JS_ToFloat64(ctx, &r, v) < 0) return false;
    out = r;
    return true;
}

inline bool from_js(JSContext* ctx, JSValueConst v, float& out) {
    double d = 0.0;
    if (!from_js(ctx, v, d)) return false;
    out = static_cast<float>(d);
    return true;
}

inline bool from_js(JSContext* ctx, JSValueConst v, std::string& out) {
    if (JS_IsUndefined(v) || JS_IsNull(v)) return false;
    size_t len = 0;
    const char* s = JS_ToCStringLen(ctx, &len, v);
    if (!s) return false;
    out.assign(s, len);
    JS_FreeCString(ctx, s);
    return true;
}

// ---------------------------------------------------------------------------
// 引擎数学类型 <-> JS 数组
// ---------------------------------------------------------------------------
inline JSValue to_js(JSContext* ctx, const math::Vector2f& v) {
    JSValue arr = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, arr, 0, to_js(ctx, v.x));
    JS_SetPropertyUint32(ctx, arr, 1, to_js(ctx, v.y));
    return arr;
}

inline JSValue to_js(JSContext* ctx, const math::Vector3f& v) {
    JSValue arr = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, arr, 0, to_js(ctx, v.x));
    JS_SetPropertyUint32(ctx, arr, 1, to_js(ctx, v.y));
    JS_SetPropertyUint32(ctx, arr, 2, to_js(ctx, v.z));
    return arr;
}

inline JSValue to_js(JSContext* ctx, const math::Quaternionf& v) {
    JSValue arr = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, arr, 0, to_js(ctx, v.x));
    JS_SetPropertyUint32(ctx, arr, 1, to_js(ctx, v.y));
    JS_SetPropertyUint32(ctx, arr, 2, to_js(ctx, v.z));
    JS_SetPropertyUint32(ctx, arr, 3, to_js(ctx, v.w));
    return arr;
}

// 列主序内存序（m[col*4+row]），16 元素平铺
inline JSValue to_js(JSContext* ctx, const math::Matrix4f& m) {
    JSValue arr = JS_NewArray(ctx);
    for (int i = 0; i < 16; ++i) {
        JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i), to_js(ctx, m.m[i]));
    }
    return arr;
}

// 数组元素读取辅助：越界/非数字返回 false
inline bool js_array_get(JSContext* ctx, JSValueConst arr, uint32_t index, double& out) {
    JSValue e = JS_GetPropertyUint32(ctx, arr, index);
    const bool ok = from_js(ctx, e, out);
    JS_FreeValue(ctx, e);
    return ok;
}

inline bool from_js(JSContext* ctx, JSValueConst v, math::Vector2f& out) {
    double x = 0.0, y = 0.0;
    if (!js_array_get(ctx, v, 0, x) || !js_array_get(ctx, v, 1, y)) return false;
    out = math::Vector2f(static_cast<float>(x), static_cast<float>(y));
    return true;
}

inline bool from_js(JSContext* ctx, JSValueConst v, math::Vector3f& out) {
    double x = 0.0, y = 0.0, z = 0.0;
    if (!js_array_get(ctx, v, 0, x) || !js_array_get(ctx, v, 1, y) || !js_array_get(ctx, v, 2, z)) {
        return false;
    }
    out = math::Vector3f(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
    return true;
}

inline bool from_js(JSContext* ctx, JSValueConst v, math::Quaternionf& out) {
    double x = 0.0, y = 0.0, z = 0.0, w = 1.0;
    if (!js_array_get(ctx, v, 0, x) || !js_array_get(ctx, v, 1, y) || !js_array_get(ctx, v, 2, z) ||
        !js_array_get(ctx, v, 3, w)) {
        return false;
    }
    out = math::Quaternionf(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z),
                            static_cast<float>(w));
    return true;
}

inline bool from_js(JSContext* ctx, JSValueConst v, math::Matrix4f& out) {
    for (int i = 0; i < 16; ++i) {
        double e = 0.0;
        if (!js_array_get(ctx, v, static_cast<uint32_t>(i), e)) return false;
        out.m[i] = static_cast<float>(e);
    }
    return true;
}

// ---------------------------------------------------------------------------
// nlohmann::json <-> JS 值（props 注入用）
// ---------------------------------------------------------------------------
inline JSValue to_js(JSContext* ctx, const nlohmann::json& j) {
    switch (j.type()) {
        case nlohmann::json::value_t::null:
            return JS_NULL;
        case nlohmann::json::value_t::boolean:
            return to_js(ctx, j.get<bool>());
        case nlohmann::json::value_t::number_integer:
        case nlohmann::json::value_t::number_unsigned:
            return to_js(ctx, j.get<int64_t>());
        case nlohmann::json::value_t::number_float:
            return to_js(ctx, j.get<double>());
        case nlohmann::json::value_t::string:
            return to_js(ctx, j.get<std::string>());
        case nlohmann::json::value_t::array: {
            JSValue arr = JS_NewArray(ctx);
            uint32_t i = 0;
            for (const auto& e : j) {
                JS_SetPropertyUint32(ctx, arr, i++, to_js(ctx, e));
            }
            return arr;
        }
        case nlohmann::json::value_t::object: {
            JSValue obj = JS_NewObject(ctx);
            for (auto it = j.begin(); it != j.end(); ++it) {
                JS_SetPropertyStr(ctx, obj, it.key().c_str(), to_js(ctx, it.value()));
            }
            return obj;
        }
        default:
            // binary / discarded 等：按 null 处理（props 场景不会出现）
            return JS_NULL;
    }
}

inline nlohmann::json from_js(JSContext* ctx, JSValueConst v) {
    if (JS_IsUndefined(v) || JS_IsNull(v)) return nlohmann::json(nullptr);
    int tag = JS_VALUE_GET_TAG(v);
    switch (tag) {
        case JS_TAG_BOOL: {
            int b = JS_ToBool(ctx, v);
            return nlohmann::json(b != 0);
        }
        case JS_TAG_INT:
            return nlohmann::json(static_cast<int64_t>(JS_VALUE_GET_INT(v)));
        case JS_TAG_FLOAT64: {
            double d = 0.0;
            JS_ToFloat64(ctx, &d, v);
            return nlohmann::json(d);
        }
        case JS_TAG_STRING: {
            std::string s;
            from_js(ctx, v, s);
            return nlohmann::json(s);
        }
        default:
            break;
    }
    if (JS_IsArray(ctx, v)) {
        nlohmann::json arr = nlohmann::json::array();
        JSValue len_v = JS_GetPropertyStr(ctx, v, "length");
        uint32_t len = 0;
        from_js(ctx, len_v, len);
        JS_FreeValue(ctx, len_v);
        for (uint32_t i = 0; i < len; ++i) {
            JSValue e = JS_GetPropertyUint32(ctx, v, i);
            arr.push_back(from_js(ctx, e));
            JS_FreeValue(ctx, e);
        }
        return arr;
    }
    if (JS_IsObject(v)) {
        nlohmann::json obj = nlohmann::json::object();
        JSPropertyEnum* tab = nullptr;
        uint32_t count = 0;
        if (JS_GetOwnPropertyNames(ctx, &tab, &count, v,
                                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
            for (uint32_t i = 0; i < count; ++i) {
                JSValue e = JS_GetProperty(ctx, v, tab[i].atom);
                const char* key = JS_AtomToCString(ctx, tab[i].atom);
                obj[key ? key : ""] = from_js(ctx, e);
                if (key) JS_FreeCString(ctx, key);
                JS_FreeValue(ctx, e);
            }
            JS_FreePropertyEnum(ctx, tab, count);
        }
        return obj;
    }
    return nlohmann::json(nullptr);
}

// ---------------------------------------------------------------------------
// 异常辅助
// ---------------------------------------------------------------------------
// 抛出 TypeError 并返回 JS_EXCEPTION（绑定参数校验用）
inline JSValue throw_type_error(JSContext* ctx, const char* msg) {
    return JS_ThrowTypeError(ctx, "%s", msg);
}

// 把挂起的异常格式化为可读字符串（含 stack）；无挂起异常时返回空串。
inline std::string take_exception_string(JSContext* ctx) {
    JSValue exc = JS_GetException(ctx);
    if (JS_IsUndefined(exc)) return {};
    std::string out;
    if (JS_IsError(ctx, exc)) {
        // Error 对象要拼「name: message」+ 换行 + stack：只取 stack 会丢掉
        // 异常主体（日志里只剩 "    at ..." 一行，看不出错因）。
        std::string name_str, msg_str;
        JSValue name_v = JS_GetPropertyStr(ctx, exc, "name");
        JSValue msg_v = JS_GetPropertyStr(ctx, exc, "message");
        from_js(ctx, name_v, name_str);
        from_js(ctx, msg_v, msg_str);
        JS_FreeValue(ctx, name_v);
        JS_FreeValue(ctx, msg_v);

        if (!name_str.empty() && !msg_str.empty()) out = name_str + ": " + msg_str;
        else if (!msg_str.empty()) out = msg_str;
        else out = name_str;

        std::string stack_str;
        JSValue stack_v = JS_GetPropertyStr(ctx, exc, "stack");
        from_js(ctx, stack_v, stack_str);
        JS_FreeValue(ctx, stack_v);
        if (!stack_str.empty()) {
            if (!out.empty()) out += '\n';
            out += stack_str;
        }
        if (out.empty()) out = "Error";
    } else {
        from_js(ctx, exc, out);
        if (out.empty()) out = "exception";
    }
    JS_FreeValue(ctx, exc);
    return out;
}

} // namespace gryce_engine::script
