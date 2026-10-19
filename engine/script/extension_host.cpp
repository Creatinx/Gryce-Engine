// ===========================================================================
// engine/script/extension_host.cpp
// 扩展 ABI 的引擎侧实现。ABI 定义见 engine/api/extension_api.h。
//
// GJsValue 与 JSValue 是同一块内存的两种视图：这里只做 memcpy，不解释
// tag/payload 的含义（QuickJS 的值表示方式可能随构建选项变化，拆字段会
// 在 NAN_BOXING 之类的配置下静默出错）。
// ===========================================================================

#include "script/extension_host.h"

#include <cstring>
#include <memory>
#include <vector>

#include "script/js_value.h"
#include "utils/glog/glog_lib.h"

#if !defined(__EMSCRIPTEN__)
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#endif

namespace gryce_engine::script {

namespace {

struct ExtFunction {
    GJsNativeFn fn = nullptr;
};

// 进程级函数表：条目只在扩展注册时追加，索引即 JS_NewCFunctionData 的 magic。
std::vector<ExtFunction> g_functions;

// 交给扩展的函数表必须活到进程结束：扩展通常会把指针缓存成静态变量
// （见 test/demo_project/extension.cpp），栈上的临时表会立刻悬空。
std::vector<std::unique_ptr<GryceExtensionApi>> g_apis;

static_assert(sizeof(JSValue) == 8 || sizeof(JSValue) == 16,
              "unexpected JSValue layout; the GJsValue bridge assumes either the "
              "struct form (tag + union, 16 bytes) or NAN_BOXING (8 bytes)");

// JSValue 有两种布局：默认的 {union, tag}（16 字节），以及开 CONFIG_NAN_BOXING
// 后的单个 64 位字（8 字节，Emscripten 构建就是这种）。GJsValue 固定 16 字节，
// 桌面下整块镜像，NAN_BOXING 下把 64 位字放进 payload。
JSValue to_jsvalue(GJsValue v) {
    JSValue out;
    if constexpr (sizeof(JSValue) == 16) {
        std::memcpy(&out, &v, sizeof(out));
    } else {
        const uint64_t bits = static_cast<uint64_t>(v.payload);
        std::memcpy(&out, &bits, sizeof(out));
    }
    return out;
}

GJsValue from_jsvalue(JSValue v) {
    GJsValue out{};
    if constexpr (sizeof(JSValue) == 16) {
        std::memcpy(&out, &v, sizeof(out));
    } else {
        uint64_t bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        out.payload = static_cast<int64_t>(bits);
    }
    return out;
}

// JSValue 带引用计数：从 GJsValue 还原后交给 JS 的返回值不能再手工释放，
// 因此调用方约定"原生函数返回的值所有权转交引擎"。

JSValue ext_trampoline(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv,
                       int magic, JSValue* /*func_data*/) {
    if (magic < 0 || magic >= static_cast<int>(g_functions.size())) {
        return JS_ThrowInternalError(ctx, "extension: invalid function index");
    }
    const GJsNativeFn fn = g_functions[static_cast<size_t>(magic)].fn;
    if (!fn) return JS_ThrowInternalError(ctx, "extension: null function pointer");

    std::vector<GJsValue> args(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) args[static_cast<size_t>(i)] = from_jsvalue(argv[i]);

    const GJsValue result = fn(ctx, from_jsvalue(this_val), argc, args.empty() ? nullptr : args.data());
    return to_jsvalue(result);
}

// engine 命名空间查找/创建（ns 为空表示 engine 本体）
JSValue engine_namespace(JSContext* ctx, const char* ns) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue engine = JS_GetPropertyStr(ctx, global, "engine");
    if (!JS_IsObject(engine)) {
        JS_FreeValue(ctx, engine);
        engine = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "engine", JS_DupValue(ctx, engine));
    }
    JS_FreeValue(ctx, global);

    if (!ns || !ns[0]) return engine;

    JSValue target = JS_GetPropertyStr(ctx, engine, ns);
    if (!JS_IsObject(target)) {
        JS_FreeValue(ctx, target);
        target = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, engine, ns, JS_DupValue(ctx, target));
    }
    JS_FreeValue(ctx, engine);
    return target;
}

void ext_add_function(void* js_ctx, const char* ns, const char* name, GJsNativeFn fn, int argc) {
    auto* ctx = static_cast<JSContext*>(js_ctx);
    if (!ctx || !fn || !name) return;

    g_functions.push_back(ExtFunction{fn});
    const int magic = static_cast<int>(g_functions.size()) - 1;

    JSValue target = engine_namespace(ctx, ns);
    JSValue f = JS_NewCFunctionData(ctx, &ext_trampoline, argc, magic, 0, nullptr);
    JS_SetPropertyStr(ctx, target, name, f);
    JS_FreeValue(ctx, target);
}

GJsValue ret_undefined(void*) {
    return from_jsvalue(JS_UNDEFINED);
}

GJsValue ret_number(void* js_ctx, double v) {
    return from_jsvalue(JS_NewFloat64(static_cast<JSContext*>(js_ctx), v));
}

GJsValue ret_bool(void* js_ctx, int v) {
    return from_jsvalue(JS_NewBool(static_cast<JSContext*>(js_ctx), v != 0));
}

GJsValue ret_string(void* js_ctx, const char* s) {
    return from_jsvalue(JS_NewString(static_cast<JSContext*>(js_ctx), s ? s : ""));
}

int arg_number(void* js_ctx, GJsValue v, double* out) {
    if (!out) return 0;
    return JS_ToFloat64(static_cast<JSContext*>(js_ctx), out, to_jsvalue(v)) == 0 ? 1 : 0;
}

int arg_string(void* js_ctx, GJsValue v, char* out, int cap) {
    if (!out || cap <= 0) return -1;
    auto* ctx = static_cast<JSContext*>(js_ctx);
    size_t len = 0;
    const char* s = JS_ToCStringLen(ctx, &len, to_jsvalue(v));
    if (!s) return -1;
    const size_t n = len < static_cast<size_t>(cap - 1) ? len : static_cast<size_t>(cap - 1);
    std::memcpy(out, s, n);
    out[n] = '\0';
    JS_FreeCString(ctx, s);
    return static_cast<int>(n);
}

int arg_bool(void* js_ctx, GJsValue v) {
    return JS_ToBool(static_cast<JSContext*>(js_ctx), to_jsvalue(v)) > 0 ? 1 : 0;
}

GJsValue ext_throw_error(void* js_ctx, const char* msg) {
    auto* ctx = static_cast<JSContext*>(js_ctx);
    JSValue exc = JS_ThrowTypeError(ctx, "%s", msg ? msg : "extension error");
    return from_jsvalue(exc);
}

void ext_log(void*, int level, const char* msg) {
    switch (level) {
        case 1: GLOG_WARN("[ext] {}", msg ? msg : ""); break;
        case 2: GLOG_ERROR("[ext] {}", msg ? msg : ""); break;
        case 3: GLOG_DEBUG("[ext] {}", msg ? msg : ""); break;
        default: GLOG_INFO("[ext] {}", msg ? msg : ""); break;
    }
}

} // namespace

GryceExtensionApi make_extension_api(JSContext* ctx) {
    GryceExtensionApi api{};
    api.version = GRYCE_EXTENSION_VERSION;
    api.size = static_cast<uint32_t>(sizeof(GryceExtensionApi));
    api.js_ctx = ctx;
    api.add_function = &ext_add_function;
    api.undefined = &ret_undefined;
    api.ret_number = &ret_number;
    api.ret_bool = &ret_bool;
    api.ret_string = &ret_string;
    api.arg_number = &arg_number;
    api.arg_string = &arg_string;
    api.arg_bool = &arg_bool;
    api.throw_error = &ext_throw_error;
    api.log = &ext_log;
    return api;
}

bool load_extension_library(JSContext* ctx, const std::string& path, std::string* error) {
#if defined(__EMSCRIPTEN__)
    (void)ctx;
    (void)path;
    if (error) *error = "extensions are not supported on the Web build";
    return false;
#else
    if (path.empty()) {
        if (error) *error = "empty extension path";
        return false;
    }

#ifdef _WIN32
    HMODULE lib = LoadLibraryA(path.c_str());
    if (!lib) {
        if (error) *error = "cannot load extension library: " + path;
        return false;
    }
    auto* reg = reinterpret_cast<GryceExtensionRegisterFn>(
        reinterpret_cast<void*>(GetProcAddress(lib, GRYCE_EXTENSION_ENTRY_NAME)));
#else
    void* lib = dlopen(path.c_str(), RTLD_NOW);
    if (!lib) {
        if (error) *error = std::string("cannot load extension library: ") + path + ": " + dlerror();
        return false;
    }
    auto* reg = reinterpret_cast<GryceExtensionRegisterFn>(dlsym(lib, GRYCE_EXTENSION_ENTRY_NAME));
#endif
    if (!reg) {
        if (error) *error = "extension entry not found: " GRYCE_EXTENSION_ENTRY_NAME;
        return false;
    }

    GryceExtensionApi* api = new GryceExtensionApi(make_extension_api(ctx));
    g_apis.emplace_back(api);
    reg(api);
    GLOG_INFO("[script] extension loaded: {}", path);
    return true;
#endif
}

} // namespace gryce_engine::script
