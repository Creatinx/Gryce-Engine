#include "script_vm.h"

#include <cstring>
#include <string>

#include <quickjs/quickjs-libc.h>

#include "utils/glog/glog_lib.h"

namespace GryceEngineUtils::ui {

// ============================================================================
// console.log 实现 — 重定向到引擎日志
// ============================================================================

namespace {

// console.log 的 C 函数实现
// 将参数拼接为字符串并输出到引擎日志
static JSValue js_console_log(JSContext* ctx, JSValueConst this_val,
                               int argc, JSValueConst* argv) {
    std::string msg;
    for (int i = 0; i < argc; ++i) {
        if (i > 0) msg += " ";
        const char* str = JS_ToCString(ctx, argv[i]);
        if (str) {
            msg += str;
            JS_FreeValue(ctx, JS_NewString(ctx, str)); // free the C string
        } else {
            msg += "undefined";
        }
        JS_FreeCString(ctx, str);
    }
    GLOG_INFO("[JS] {}", msg);
    return JS_UNDEFINED;
}

// console.warn
static JSValue js_console_warn(JSContext* ctx, JSValueConst this_val,
                                int argc, JSValueConst* argv) {
    std::string msg;
    for (int i = 0; i < argc; ++i) {
        if (i > 0) msg += " ";
        const char* str = JS_ToCString(ctx, argv[i]);
        if (str) {
            msg += str;
        }
        JS_FreeCString(ctx, str);
    }
    GLOG_WARN("[JS] {}", msg);
    return JS_UNDEFINED;
}

// console.error
static JSValue js_console_error(JSContext* ctx, JSValueConst this_val,
                                 int argc, JSValueConst* argv) {
    std::string msg;
    for (int i = 0; i < argc; ++i) {
        if (i > 0) msg += " ";
        const char* str = JS_ToCString(ctx, argv[i]);
        if (str) {
            msg += str;
        }
        JS_FreeCString(ctx, str);
    }
    GLOG_ERROR("[JS] {}", msg);
    return JS_UNDEFINED;
}

// 注册 console 对象到全局作用域
static void init_console(JSContext* ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue console = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, console, "log",
                      JS_NewCFunction(ctx, js_console_log, "log", 1));
    JS_SetPropertyStr(ctx, console, "warn",
                      JS_NewCFunction(ctx, js_console_warn, "warn", 1));
    JS_SetPropertyStr(ctx, console, "error",
                      JS_NewCFunction(ctx, js_console_error, "error", 1));

    JS_SetPropertyStr(ctx, global, "console", console);
    JS_FreeValue(ctx, global);
}

} // namespace

// ============================================================================
// ScriptVM 实现
// ============================================================================

ScriptVM::~ScriptVM() {
    shutdown();
}

bool ScriptVM::init() {
    if (initialized_) return true;

    // 创建运行时
    rt_ = JS_NewRuntime();
    if (!rt_) {
        GLOG_ERROR("ScriptVM: failed to create JSRuntime");
        return false;
    }

    // 设置内存限制（64MB）和栈大小
    JS_SetMemoryLimit(rt_, 64 * 1024 * 1024);
    JS_SetMaxStackSize(rt_, 256 * 1024);

    // 创建上下文
    ctx_ = JS_NewContext(rt_);
    if (!ctx_) {
        GLOG_ERROR("ScriptVM: failed to create JSContext");
        JS_FreeRuntime(rt_);
        rt_ = nullptr;
        return false;
    }

    // 注册 console.log / warn / error
    init_console(ctx_);

    initialized_ = true;
    GLOG_INFO("ScriptVM initialized");
    return true;
}

void ScriptVM::shutdown() {
    if (!initialized_) return;

    if (ctx_) {
        JS_FreeContext(ctx_);
        ctx_ = nullptr;
    }
    if (rt_) {
        JS_FreeRuntime(rt_);
        rt_ = nullptr;
    }

    initialized_ = false;
    GLOG_INFO("ScriptVM shutdown");
}

ScriptResult ScriptVM::eval(const std::string& code, const std::string& filename) {
    if (!initialized_ || !ctx_) {
        return {false, "", "ScriptVM not initialized"};
    }

    JSValue result = JS_Eval(ctx_, code.c_str(), code.size(),
                              filename.c_str(), JS_EVAL_TYPE_GLOBAL);

    ScriptResult sr = jsvalue_to_result(ctx_, result);
    JS_FreeValue(ctx_, result);
    return sr;
}

ScriptResult ScriptVM::call_function(const std::string& name,
                                      const std::vector<JSValueWrapper>& args) {
    if (!initialized_ || !ctx_) {
        return {false, "", "ScriptVM not initialized"};
    }

    JSValue global = JS_GetGlobalObject(ctx_);
    JSValue func = JS_GetPropertyStr(ctx_, global, name.c_str());

    if (JS_IsException(func)) {
        JS_FreeValue(ctx_, func);
        JS_FreeValue(ctx_, global);
        return {false, "", "Failed to get function '" + name + "'"};
    }

    // 检查是否为函数
    if (!JS_IsFunction(ctx_, func)) {
        JS_FreeValue(ctx_, func);
        JS_FreeValue(ctx_, global);
        return {false, "", "'" + name + "' is not a function"};
    }

    // 准备参数
    std::vector<JSValue> js_args;
    js_args.reserve(args.size());
    for (const auto& arg : args) {
        js_args.push_back(to_js_value(ctx_, arg));
    }

    // 调用函数
    JSValue result = JS_Call(ctx_, func, global,
                              static_cast<int>(js_args.size()),
                              js_args.empty() ? nullptr : js_args.data());

    // 释放参数
    for (auto& jsv : js_args) {
        JS_FreeValue(ctx_, jsv);
    }

    ScriptResult sr = jsvalue_to_result(ctx_, result);
    JS_FreeValue(ctx_, result);
    JS_FreeValue(ctx_, func);
    JS_FreeValue(ctx_, global);
    return sr;
}

void ScriptVM::register_function(const std::string& name, JSCFunction* func) {
    if (!initialized_ || !ctx_ || !func) return;

    JSValue global = JS_GetGlobalObject(ctx_);
    JSValue js_func = JS_NewCFunction(ctx_, func, name.c_str(),
                                       static_cast<int>(name.size()));
    JS_SetPropertyStr(ctx_, global, name.c_str(), js_func);
    JS_FreeValue(ctx_, global);

    GLOG_INFO("ScriptVM: registered function '{}'", name);
}

// ============================================================================
// 辅助方法
// ============================================================================

JSValue ScriptVM::to_js_value(JSContext* ctx, const JSValueWrapper& wrapper) {
    switch (wrapper.type) {
        case JSValueWrapper::Type::Undefined:
            return JS_UNDEFINED;
        case JSValueWrapper::Type::Null:
            return JS_NULL;
        case JSValueWrapper::Type::Bool:
            return JS_NewBool(ctx, wrapper.bool_val);
        case JSValueWrapper::Type::Int:
            return JS_NewInt32(ctx, wrapper.int_val);
        case JSValueWrapper::Type::Float:
            return JS_NewFloat64(ctx, wrapper.float_val);
        case JSValueWrapper::Type::String:
            return JS_NewString(ctx, wrapper.str_val.c_str());
    }
    return JS_UNDEFINED;
}

ScriptResult ScriptVM::jsvalue_to_result(JSContext* ctx, JSValue val) {
    ScriptResult sr;

    if (JS_IsException(val)) {
        sr.success = false;
        JSValue exc = JS_GetException(ctx);
        const char* str = JS_ToCString(ctx, exc);
        sr.error_msg = str ? str : "Unknown error";
        JS_FreeCString(ctx, str);
        JS_FreeValue(ctx, exc);
        return sr;
    }

    sr.success = true;
    const char* str = JS_ToCString(ctx, val);
    sr.result = str ? str : "";
    JS_FreeCString(ctx, str);
    return sr;
}

} // namespace GryceEngineUtils::ui