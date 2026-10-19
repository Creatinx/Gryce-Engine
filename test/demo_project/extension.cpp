// ===========================================================================
// test/demo_project/extension.cpp — 项目原生扩展样例
//
// 扩展的职责是**接口层**，不是业务层：引擎里已经实现、但脚本层（engine.*）
// 还没有开放的 C API，在这里包成 JS 接口交给 main.js 用。关卡逻辑、相机
// 控制、UI 流程仍然写在 main.js；扩展只做「取参数 → 调引擎 → 造返回值」。
//
// 样例暴露的三项能力引擎里早已实现（engine/api/entity_api.h、scene_api.h），
// 但 JS 绑定没有对应函数，正是扩展要补的那类缺口：
//
//   engine.entity.toJson(handle)      GEntity_ExportJson   实体子树序列化
//   engine.entity.path(handle)        GEntity_GetPath      层级路径（A/B/C）
//   engine.scene.pick(sx, sy, w, h)   GScene_PickScreen    屏幕射线拾取
//
// 调用引擎能力走的是 C ABI：include engine/api/*.h 并链接引擎导入库。
// 不要 include 引擎内部的 C++ 头（scene/entity.h 之类），那会把扩展绑死
// 在某一次编译的 C++ ABI 上，升级引擎就得重编。
//
// 编译：cmake --build build --target GryceDemoExtension
// 手写：g++ -shared -O2 extension.cpp -I <engine> -L <build> -lGryceCore -o extension.dll
// 加载：放 <project>/extension.dll，或 GryceApp --extension <path>
// Web：动态库在 wasm 侧不可用，扩展只在桌面生效。
// ===========================================================================

#include "api/extension_api.h"

#include "api/entity_api.h"
#include "api/scene_api.h"

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace {

// 注册入口每次重建 context 都会调用一次，指针必须覆盖刷新；宿主保活到退出。
GryceExtensionApi* g_api = nullptr;

GJsValue fail(const char* message) {
    return g_api->throw_error(g_api->js_ctx, message);
}

// 实体句柄在 JS 侧是 entity.id() 返回的整数（引擎里是 int）。
bool read_handle(const GJsValue* argv, int argc, int index, GEntityHandle* out) {
    if (!out || argc <= index) return false;
    double v = 0.0;
    if (!g_api->arg_number(g_api->js_ctx, argv[index], &v)) return false;
    if (!(v >= 1.0) || v > 2147483647.0) return false;   // 也挡掉 NaN
    *out = static_cast<GEntityHandle>(v);
    return true;
}

// --- engine.ext.info() -----------------------------------------------------
GJsValue js_ext_info(void*, GJsValue, int, const GJsValue*) {
    return g_api->ret_string(g_api->js_ctx, "demo-extension 1.0 (engine adapters)");
}

// --- engine.entity.toJson(handle) -> string --------------------------------
// 引擎实现：GEntity_ExportJson 把实体子树序列化成 {version,type,name,entities[]}。
// JS 绑定只开放了导入侧（engine.entity.spawn(json)），导出侧由扩展补上。
GJsValue js_entity_to_json(void*, GJsValue, int argc, const GJsValue* argv) {
    GEntityHandle h = 0;
    if (!read_handle(argv, argc, 0, &h)) {
        return fail("engine.entity.toJson(handle) expects entity.id()");
    }
    std::vector<char> buf(64 * 1024);
    const int n = GEntity_ExportJson(h, buf.data(), static_cast<int>(buf.size()));
    if (n < 0) {
        return fail("engine.entity.toJson: invalid handle, or subtree exceeds 64 KiB");
    }
    return g_api->ret_string(g_api->js_ctx, buf.data());
}

// --- engine.entity.path(handle) -> string ---------------------------------
GJsValue js_entity_path(void*, GJsValue, int argc, const GJsValue* argv) {
    GEntityHandle h = 0;
    if (!read_handle(argv, argc, 0, &h)) {
        return fail("engine.entity.path(handle) expects entity.id()");
    }
    char path[1024] = {};
    const int n = GEntity_GetPath(h, path, static_cast<int>(sizeof(path)));
    if (n < 0) return fail("engine.entity.path: invalid handle");
    return g_api->ret_string(g_api->js_ctx, path);
}

// --- engine.scene.pick(sx, sy, viewport_w, viewport_h [, camera_id]) -------
// 引擎实现：GScene_PickScreen 反投影成世界射线做求交，返回命中的实体句柄，
// 0 表示未命中。camera_id 省略时引擎自己找场景里第一个启用的相机。
GJsValue js_scene_pick(void*, GJsValue, int argc, const GJsValue* argv) {
    if (argc < 4) {
        return fail("engine.scene.pick(sx, sy, width, height [, cameraId]) expects 4 arguments");
    }
    double v[4] = {};
    for (int i = 0; i < 4; ++i) {
        if (!g_api->arg_number(g_api->js_ctx, argv[i], &v[i])) {
            return fail("engine.scene.pick: sx, sy, width and height must be numbers");
        }
    }
    GEntityHandle camera = 0;
    if (argc >= 5) {
        if (!read_handle(argv, argc, 4, &camera)) {
            return fail("engine.scene.pick: cameraId must be entity.id() or omitted");
        }
    }
    const GEntityHandle hit = GScene_PickScreen(
        static_cast<float>(v[0]), static_cast<float>(v[1]),
        static_cast<int>(v[2]), static_cast<int>(v[3]), camera);
    return g_api->ret_number(g_api->js_ctx, static_cast<double>(hit));
}

} // namespace

GRYCE_EXTENSION_ENTRY {
    if (!api || api->version != GRYCE_EXTENSION_VERSION) return;
    g_api = api;   // 每次注册覆盖：新 context，新函数表

    api->add_function(api->js_ctx, "ext", "info", &js_ext_info, 0);

    // 挂到 JS 绑定已有的命名空间上，脚本侧看起来就是内置接口。
    api->add_function(api->js_ctx, "entity", "toJson", &js_entity_to_json, 1);
    api->add_function(api->js_ctx, "entity", "path", &js_entity_path, 1);
    api->add_function(api->js_ctx, "scene", "pick", &js_scene_pick, 4);
}
