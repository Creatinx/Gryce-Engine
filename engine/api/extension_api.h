// ===========================================================================
// engine/api/extension_api.h
// 原生扩展 ABI：项目用自己的 extension.cpp（编成动态库）给 main.js 增补
// engine.xxx 接口，不需要重新编译引擎，也不需要把项目代码塞进 main.cpp。
//
// 形态：宿主启动时 dlopen <project>/extension.dll（Linux: .so），取
// "gryce_register_extension" 入口，把一张函数表传进去。扩展只依赖本头
// （纯 C，不含 QuickJS 头），因此不与引擎的 C++ ABI / 编译选项耦合。
//
// 值的表示：GJsValue 是 JSValue 的不透明镜像（16 字节，按值传），只能用
// api 里的 arg_* / ret_* 构造和读取，不要自己拆。
//
// 加载时机：main.js 求值之前，因此 main.js 顶层即可使用扩展接口；热重载
// main.js 不会重复加载扩展（扩展与进程同生命周期）。
//
// 生命周期：传给注册入口的 api 指针在进程内一直有效，可以安全地存成扩展
// 自己的静态变量（常见做法：后续原生函数要用它构造返回值）。
// ===========================================================================

#ifndef GRYCE_EXTENSION_API_H
#define GRYCE_EXTENSION_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GRYCE_EXTENSION_VERSION 1u

// JSValue 的不透明按值镜像。字段顺序与 JSValue 的内存布局一致
// （payload 在前、tag 在后），但语义由引擎定义：扩展不要直接读写。
typedef struct GJsValue {
    int64_t payload;
    int64_t tag;
} GJsValue;

// 原生函数签名：js_ctx 由宿主持有，不要保存；this_val/argv 只在调用期间有效。
typedef GJsValue (*GJsNativeFn)(void* js_ctx, GJsValue this_val, int argc,
                                const GJsValue* argv);

typedef struct GryceExtensionApi {
    uint32_t version;  // GRYCE_EXTENSION_VERSION；扩展应校验
    uint32_t size;     // 本结构大小（字节），用于向后兼容的字段探测

    void* js_ctx;      // 本次注册所属的 JSContext（传给原生函数）

    // 在 engine.<ns> 上挂一个原生函数；ns 为 NULL 或 "" 表示 engine 本体。
    void (*add_function)(void* js_ctx, const char* ns, const char* name,
                         GJsNativeFn fn, int argc);

    // 返回值构造
    GJsValue (*undefined)(void* js_ctx);
    GJsValue (*ret_number)(void* js_ctx, double v);
    GJsValue (*ret_bool)(void* js_ctx, int v);
    GJsValue (*ret_string)(void* js_ctx, const char* s);

    // 参数读取：成功返回 1；类型不符返回 0（不修改 out）。arg_string 在
    // 缓冲区不足时截断并返回实际写入长度（不含结尾 0），返回 -1 表示失败。
    int (*arg_number)(void* js_ctx, GJsValue v, double* out);
    int (*arg_string)(void* js_ctx, GJsValue v, char* out, int cap);
    int (*arg_bool)(void* js_ctx, GJsValue v);

    // 抛出 Error 并返回被标记为异常的 GJsValue（直接 return 即可）。
    GJsValue (*throw_error)(void* js_ctx, const char* msg);

    // 写一行引擎日志（level: 0=info 1=warn 2=error 3=debug）。
    void (*log)(void* js_ctx, int level, const char* msg);
} GryceExtensionApi;

typedef void (*GryceExtensionRegisterFn)(GryceExtensionApi* api);

#define GRYCE_EXTENSION_ENTRY_NAME "gryce_register_extension"

#ifdef _WIN32
#define GRYCE_EXTENSION_ENTRY \
    extern "C" __declspec(dllexport) void gryce_register_extension(GryceExtensionApi* api)
#else
#define GRYCE_EXTENSION_ENTRY \
    extern "C" __attribute__((visibility("default"))) void gryce_register_extension( \
        GryceExtensionApi* api)
#endif

#ifdef __cplusplus
}
#endif

#endif // GRYCE_EXTENSION_API_H
