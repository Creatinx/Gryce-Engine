#include "script_vm.h"

#include <cstring>
#include <regex>
#include <string>

#include <quickjs/quickjs-libc.h>

#include "utils/glog/glog_lib.h"

namespace GryceEngineUtils::script {

// ============================================================================
// console.log 实现 — 重定向到引擎日志
// ============================================================================

namespace {

static JSValue js_console_log(JSContext* ctx, JSValueConst this_val,
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
    GLOG_INFO("[JS] {}", msg);
    return JS_UNDEFINED;
}

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
// 模块代码准备 — 将 ES Module 语法的 export 转换为 exports.xxx 赋值，
// 并包装为 IIFE（eval_module 与 compile_script 共用，保证字节码语义一致）
// ============================================================================

namespace {

std::string wrap_module_code(const std::string& code) {
    std::string processed = code;

    // 1. export function name(...) -> exports.name = function(...)
    processed = std::regex_replace(processed,
        std::regex("export\\s+function\\s+(\\w+)"),
        "exports.$1 = function");

    // 2. export const/let/var name -> exports.name
    processed = std::regex_replace(processed,
        std::regex("export\\s+(const|let|var)\\s+"),
        "exports.");

    // 3. export default expr -> exports.default = expr
    processed = std::regex_replace(processed,
        std::regex("export\\s+default\\s+"),
        "exports.default = ");

    // 4. 处理 export { name1, name2 } 形式
    {
        std::regex export_brace_regex("export\\s*\\{\\s*([^}]+)\\s*\\}\\s*;?");
        std::string result;
        auto begin = std::sregex_iterator(processed.begin(), processed.end(), export_brace_regex);
        auto end = std::sregex_iterator();
        size_t last_pos = 0;
        for (auto it = begin; it != end; ++it) {
            result += processed.substr(last_pos, it->position() - last_pos);
            std::string replacement;
            std::string list = (*it)[1].str();
            size_t start = 0;
            while (start < list.size()) {
                while (start < list.size() && (list[start] == ' ' || list[start] == '\t'))
                    ++start;
                if (start >= list.size()) break;
                size_t end = start;
                while (end < list.size() && list[end] != ',')
                    ++end;
                std::string name = list.substr(start, end - start);
                while (!name.empty() && (name.back() == ' ' || name.back() == '\t'))
                    name.pop_back();
                if (!name.empty()) {
                    replacement += "exports." + name + " = " + name + "; ";
                }
                start = end + 1;
            }
            result += replacement;
            last_pos = it->position() + it->length();
        }
        result += processed.substr(last_pos);
        processed = result;
    }

    std::string wrapped;
    wrapped += "(function(exports) {\n";
    wrapped += processed;
    wrapped += "\nreturn exports;\n})({})";
    return wrapped;
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

    rt_ = JS_NewRuntime();
    if (!rt_) {
        GLOG_ERROR("ScriptVM: failed to create JSRuntime");
        return false;
    }

    JS_SetMemoryLimit(rt_, 64 * 1024 * 1024);
    JS_SetMaxStackSize(rt_, 256 * 1024);

    ctx_ = JS_NewContext(rt_);
    if (!ctx_) {
        GLOG_ERROR("ScriptVM: failed to create JSContext");
        JS_FreeRuntime(rt_);
        rt_ = nullptr;
        return false;
    }

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

ScriptResult ScriptVM::eval_module(const std::string& code, const std::string& filename,
                                    JSValue* out_module_ns) {
    if (!initialized_ || !ctx_) {
        return {false, "", "ScriptVM not initialized"};
    }

    // 使用 IIFE + exports 对象模拟 ES Module（与 compile_script 共用转换逻辑）
    const std::string wrapped_code = wrap_module_code(code);

    // 以全局模式执行包装后的代码
    JSValue result = JS_Eval(ctx_, wrapped_code.c_str(), wrapped_code.size(),
                              filename.c_str(), JS_EVAL_TYPE_GLOBAL);

    ScriptResult sr = jsvalue_to_result(ctx_, result);

    if (sr.success && out_module_ns) {
        if (JS_IsObject(result)) {
            *out_module_ns = JS_DupValue(ctx_, result);
        } else {
            *out_module_ns = JS_UNDEFINED;
        }
    } else if (out_module_ns) {
        *out_module_ns = JS_UNDEFINED;
    }

    JS_FreeValue(ctx_, result);
    return sr;
}

std::vector<uint8_t> ScriptVM::compile_script(const std::string& code, const std::string& filename,
                                              bool as_module, std::string* error) {
    if (!initialized_ || !ctx_) {
        if (error) *error = "ScriptVM not initialized";
        return {};
    }

    const std::string src = as_module ? wrap_module_code(code) : code;

    // 仅编译不执行；结果是被序列化前的一次性函数字节码对象
    JSValue bc = JS_Eval(ctx_, src.c_str(), src.size(), filename.c_str(),
                         JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(bc)) {
        const std::string err = consume_exception();
        if (error) *error = err;
        JS_FreeValue(ctx_, bc);
        return {};
    }

    size_t size = 0;
    uint8_t* buf = JS_WriteObject(ctx_, &size, bc, JS_WRITE_OBJ_BYTECODE);
    JS_FreeValue(ctx_, bc);
    if (!buf) {
        if (error) *error = "JS_WriteObject failed";
        return {};
    }

    std::vector<uint8_t> out(buf, buf + size);
    js_free(ctx_, buf);
    return out;
}

ScriptResult ScriptVM::eval_bytecode(const std::vector<uint8_t>& bytecode, const std::string& filename,
                                     JSValue* out_module_ns) {
    if (!initialized_ || !ctx_) {
        return {false, "", "ScriptVM not initialized"};
    }
    if (bytecode.empty()) {
        return {false, "", "empty bytecode"};
    }

    JSValue bc = JS_ReadObject(ctx_, bytecode.data(), bytecode.size(), JS_READ_OBJ_BYTECODE);
    if (JS_IsException(bc)) {
        JS_FreeValue(ctx_, bc);
        return {false, "", "JS_ReadObject failed: " + consume_exception()};
    }

    // JS_EvalFunction 消费 bc（内部释放）
    JSValue result = JS_EvalFunction(ctx_, bc);

    ScriptResult sr = jsvalue_to_result(ctx_, result);

    if (sr.success && out_module_ns) {
        if (JS_IsObject(result)) {
            *out_module_ns = JS_DupValue(ctx_, result);
        } else {
            *out_module_ns = JS_UNDEFINED;
        }
    } else if (out_module_ns) {
        *out_module_ns = JS_UNDEFINED;
    }

    JS_FreeValue(ctx_, result);
    return sr;
}

ScriptResult ScriptVM::call_module_function(JSValue module_ns, const char* func_name,
                                             const std::vector<JSValueWrapper>& args) {
    if (!initialized_ || !ctx_) {
        return {false, "", "ScriptVM not initialized"};
    }

    // 从模块导出中获取函数
    JSValue func = JS_GetPropertyStr(ctx_, module_ns, func_name);
    if (JS_IsException(func)) {
        JS_FreeValue(ctx_, func);
        return {false, "", "Failed to get export '" + std::string(func_name) + "'"};
    }

    // 检查 module_ns 是否有效
    if (JS_IsUndefined(module_ns)) {
        JS_FreeValue(ctx_, func);
        return {false, "", "module_ns is undefined"};
    }

    if (JS_IsUndefined(func)) {
        JS_FreeValue(ctx_, func);
        // 尝试获取模块命名空间中的所有属性名
        JSPropertyEnum* props = nullptr;
        uint32_t props_len = 0;
        if (JS_GetOwnPropertyNames(ctx_, &props, &props_len, module_ns,
                                    JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
            std::string names = "Available exports: ";
            for (uint32_t i = 0; i < props_len; ++i) {
                const char* s = JS_AtomToCString(ctx_, props[i].atom);
                if (s) { names += s; names += " "; JS_FreeCString(ctx_, s); }
            }
            JS_FreePropertyEnum(ctx_, props, props_len);
            return {false, "", "'" + std::string(func_name) + "' not found in module. " + names};
        }
        return {false, "", "'" + std::string(func_name) + "' not found in module namespace"};
    }

    if (!JS_IsFunction(ctx_, func)) {
        JS_FreeValue(ctx_, func);
        return {false, "", "'" + std::string(func_name) + "' is not a function"};
    }

    // 准备参数
    std::vector<JSValue> js_args;
    js_args.reserve(args.size());
    for (const auto& arg : args) {
        js_args.push_back(to_js_value(ctx_, arg));
    }

    JSValue result = JS_Call(ctx_, func, JS_UNDEFINED,
                              static_cast<int>(js_args.size()),
                              js_args.empty() ? nullptr : js_args.data());

    for (auto& jsv : js_args) {
        JS_FreeValue(ctx_, jsv);
    }

    ScriptResult sr = jsvalue_to_result(ctx_, result);
    JS_FreeValue(ctx_, result);
    JS_FreeValue(ctx_, func);
    return sr;
}

JSValue ScriptVM::get_module_exports(JSValue module_ns) {
    // 模块命名空间对象本身就是导出集合
    // 可以直接通过 JS_GetPropertyStr 访问导出成员
    return JS_DupValue(ctx_, module_ns);
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

    if (!JS_IsFunction(ctx_, func)) {
        JS_FreeValue(ctx_, func);
        JS_FreeValue(ctx_, global);
        return {false, "", "'" + name + "' is not a function"};
    }

    std::vector<JSValue> js_args;
    js_args.reserve(args.size());
    for (const auto& arg : args) {
        js_args.push_back(to_js_value(ctx_, arg));
    }

    JSValue result = JS_Call(ctx_, func, global,
                              static_cast<int>(js_args.size()),
                              js_args.empty() ? nullptr : js_args.data());

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

std::string ScriptVM::format_exception(JSValue exception_val) {
    if (!ctx_) return "No context";

    JSValue val = JS_GetException(ctx_);
    std::string result;

    const char* str = JS_ToCString(ctx_, val);
    if (str) {
        result = str;
        JS_FreeCString(ctx_, str);
    } else {
        result = "Unknown exception";
    }

    JS_FreeValue(ctx_, val);
    return result;
}

std::string ScriptVM::consume_exception() {
    if (!ctx_) return "No context";

    JSValue val = JS_GetException(ctx_);
    if (JS_IsUndefined(val)) {
        JS_FreeValue(ctx_, val);
        return "";
    }

    std::string result;
    const char* str = JS_ToCString(ctx_, val);
    if (str) {
        result = str;
        JS_FreeCString(ctx_, str);
    } else {
        result = "Unknown exception";
    }

    JS_FreeValue(ctx_, val);
    return result;
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

} // namespace GryceEngineUtils::script