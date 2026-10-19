// extension.cpp — 项目原生扩展样例（编成动态库，宿主启动时加载）
//
// 用途：main.js 里需要的、但引擎没提供的能力（项目私有格式、平台接口、
// 性能敏感的算法），在 C++ 里实现后挂到 engine.<命名空间> 上，JS 侧像内置
// API 一样直接调用。不需要重新编译引擎，也不需要在项目里写 main.cpp。
//
// 编译（与引擎分开构建，只依赖这个头文件）：
//   g++ -shared -O2 extension.cpp -I<engine>/engine -o extension.dll
// 或直接用仓库里的 GryceDemoExtension 目标（输出到构建目录）。
//
// 加载：默认找 <project>/extension.dll；也可 GryceApp --extension <path>。
// 注意：Web 构建不支持动态库，扩展只在桌面生效。

#include "api/extension_api.h"

#include <cstdio>
#include <string>

namespace {

constexpr const char* kVersion = "demo-extension 1.0";

// 动态库没有引擎的符号表，返回值构造 / 参数读取 / 抛错全部走传入的函数表。
// 函数表本身在注册时缓存下来，供后续调用使用。
GryceExtensionApi* g_api = nullptr;

GJsValue js_demo_version(void*, GJsValue, int, const GJsValue*) {
    return g_api->ret_string(g_api->js_ctx, kVersion);
}

GJsValue js_demo_add(void*, GJsValue, int argc, const GJsValue* argv) {
    double a = 0.0;
    double b = 0.0;
    if (argc < 2 || !g_api->arg_number(g_api->js_ctx, argv[0], &a) ||
        !g_api->arg_number(g_api->js_ctx, argv[1], &b)) {
        return g_api->throw_error(g_api->js_ctx, "engine.game.add(a, b) expects two numbers");
    }
    return g_api->ret_number(g_api->js_ctx, a + b);
}

GJsValue js_demo_greet(void*, GJsValue, int argc, const GJsValue* argv) {
    char name[64] = {};
    if (argc < 1 || g_api->arg_string(g_api->js_ctx, argv[0], name, sizeof(name)) < 0) {
        return g_api->throw_error(g_api->js_ctx, "engine.game.greet(name) expects a string");
    }
    const std::string msg = std::string("hello, ") + name;
    g_api->log(g_api->js_ctx, 0, msg.c_str());
    return g_api->ret_string(g_api->js_ctx, msg.c_str());
}

} // namespace

GRYCE_EXTENSION_ENTRY {
    if (!api || api->version != GRYCE_EXTENSION_VERSION) return;
    g_api = api;

    api->add_function(api->js_ctx, "game", "version", &js_demo_version, 0);
    api->add_function(api->js_ctx, "game", "add", &js_demo_add, 2);
    api->add_function(api->js_ctx, "game", "greet", &js_demo_greet, 1);
}
