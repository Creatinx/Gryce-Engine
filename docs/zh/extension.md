# 原生扩展（extension.cpp）

本文是原生扩展的完整用法与 ABI 参考：如何把**引擎已实现、但脚本层还没开放**的能力暴露成 `engine.*` 接口、ABI 每个字段的精确语义、值传递规则、编译与分发方式，以及已知陷阱。

扩展面向**入口脚本**（[app-entry.md](app-entry.md)）。组件级脚本（[scripting.md](scripting.md)）不装载扩展：它随场景在任意进程里实例化，无法保证对应动态库存在。

## 1. 定位

扩展不是「在 C++ 里写游戏逻辑」的口子，而是**接口层**：引擎里已经实现、但脚本层（`engine.*`）还没有开放的 C API，由项目侧包成 JS 接口补上。典型场景是引擎能力已经就绪、公开的 JS 绑定还没跟上——底层函数在 `engine/api/` 里能找到，脚本里却调不到。

| 需求 | 合适的位置 |
| --- | --- |
| 关卡逻辑、相机控制、UI 流程、参数调节 | `main.js` |
| 引擎已实现、JS 绑定未开放的 C API（拾取、序列化、预制体、层级路径……） | 扩展 |
| 调用项目私有格式解析、平台接口、第三方 SDK | 扩展 |
| 对性能敏感的纯计算（逐点运算、批量几何处理） | 扩展 |
| 修改渲染管线、资源导入、ECS 行为 | 改引擎，不是扩展 |

判断标准：**能用一条 `GXxx_Yyy()` 直接表达的是扩展的活；需要 if/else 描述游戏规则的，是 `main.js` 的活。**

### 能拿到什么

扩展运行在宿主进程内，可以 `#include` 引擎的 C 接口头并链接引擎导入库，直接调用 `GRYCE_*_API` 函数：

| 头文件 | 典型能力 |
| --- | --- |
| `api/entity_api.h` | 父子/兄弟遍历、世界与局部变换、`GEntity_ExportJson` / `GEntity_ImportJson`、预制体保存与实例化 |
| `api/scene_api.h` | 加载/保存/新建场景、`GScene_PickScreen` / `GScene_PickRay` 拾取、场景模式 |
| `api/core_api.h` | 命令面、播放态查询、实体选中/场景加载等回调注册 |
| `api/asset_api.h`、`api/component_api.h`、`api/animator_api.h`、`api/physics_api.h` | 资源、组件、动画、物理 |

可调用面以 `engine/api/` 下实际导出的函数为准。**不要 include 引擎内部的 C++ 头**（`scene/entity.h`、`render/render_pipeline.h` 等），那会把扩展绑死在某一次编译的 C++ ABI 上，引擎一改就得重编。

### 不做什么

- 不重新实现引擎已有的能力。引擎里有就直接调，扩展只负责参数转换与返回值构造。
- 不持有业务状态。状态放 `main.js`，扩展的静态变量只用于缓存（例如函数表指针）。
- 不替代 JS 绑定。能力稳定后应上移到 `engine/script/js_app_bindings.cpp`，扩展只作过渡层。

虚表的职责只是 **JS 值通道**（取标量参数、构造标量/字符串返回值、抛错、写日志），引擎能力不经过虚表，而是靠链接直接调用。

## 2. 加载机制

| 环节 | 行为 |
| --- | --- |
| 查找路径 | `--extension <path>` 显式指定优先；否则在 `<project>/extension.dll`（Linux `extension.so`）存在时加载 |
| 禁用 | `--no-extension` 关闭**默认路径查找**；已显式给出的 `--extension` 仍然生效 |
| 时机 | 在 `on_context_created` 回调里，顺序为「安装应用绑定 → 加载扩展 → 求值 `main.js`」；因此 `main.js` 顶层代码即可调用扩展接口 |
| 失败 | 记录 `[app] extension load failed: ...` 并继续运行，`main.js` 照常启动；脚本侧应按 `typeof` 探测可选能力 |
| 卸载 | 不卸载。库与进程同生命周期 |
| 重复注册 | `main.js` 热重载会重建 `JSContext`，加载流程随之重放：`dlopen` / `LoadLibrary` 命中已加载的模块（引用计数不归零），但会新建一张 `GryceExtensionApi` 并再次调用注册入口 |

「重复注册」是文档里最容易踩错的一条：**注册入口可能被调用多次，每次拿到的 `api` 指针是新对象**。宿主持有所有 `api` 对象直到进程结束，因此旧指针不会悬空；但旧指针里的 `js_ctx` 指向已销毁的 context，用它注册函数会把函数装进死 context。正确写法是每次注册都刷新缓存：

```cpp
GRYCE_EXTENSION_ENTRY {
    if (!api || api->version != GRYCE_EXTENSION_VERSION) return;
    g_api = api;   // 每次覆盖：新 context、新 api
    api->add_function(api->js_ctx, "game", "version", &js_version, 0);
}
```

扩展自身的静态变量（计数器、句柄、缓存）**不会**因重新注册而重置，与「进程级单例」一致。

## 3. 示例：把引擎已有能力接成 JS 接口

仓库样例 `test/demo_project/extension.cpp` 暴露三项**引擎里已经实现、JS 绑定没有**的功能：

| 扩展接口 | 引擎实现 | 说明 |
| --- | --- | --- |
| `engine.entity.toJson(handle)` | `GEntity_ExportJson` | 实体子树序列化为 JSON（JS 侧只开放了导入方向的 `engine.entity.spawn(json)`） |
| `engine.entity.path(handle)` | `GEntity_GetPath` | 层级路径，形如 `Root/Child/Leaf` |
| `engine.scene.pick(sx, sy, w, h[, cameraId])` | `GScene_PickScreen` | 屏幕坐标反投影求交，返回实体句柄，0 表示未命中 |

```cpp
#include "api/extension_api.h"

#include "api/entity_api.h"
#include "api/scene_api.h"

#include <vector>

namespace {

GryceExtensionApi* g_api = nullptr;   // 注册时刷新，进程内一直有效

GJsValue fail(const char* message) {
    return g_api->throw_error(g_api->js_ctx, message);
}

// 句柄在 JS 侧是 entity.id() 返回的整数（引擎里是 int）。
bool read_handle(const GJsValue* argv, int argc, int index, GEntityHandle* out) {
    if (!out || argc <= index) return false;
    double v = 0.0;
    if (!g_api->arg_number(g_api->js_ctx, argv[index], &v)) return false;
    if (!(v >= 1.0) || v > 2147483647.0) return false;   // 也挡掉 NaN
    *out = static_cast<GEntityHandle>(v);
    return true;
}

GJsValue js_entity_path(void*, GJsValue, int argc, const GJsValue* argv) {
    GEntityHandle h = 0;
    if (!read_handle(argv, argc, 0, &h)) return fail("engine.entity.path(handle) expects entity.id()");
    char path[1024] = {};
    if (GEntity_GetPath(h, path, static_cast<int>(sizeof(path))) < 0) {
        return fail("engine.entity.path: invalid handle");
    }
    return g_api->ret_string(g_api->js_ctx, path);
}

GJsValue js_entity_to_json(void*, GJsValue, int argc, const GJsValue* argv) {
    GEntityHandle h = 0;
    if (!read_handle(argv, argc, 0, &h)) return fail("engine.entity.toJson(handle) expects entity.id()");
    std::vector<char> buf(64 * 1024);
    if (GEntity_ExportJson(h, buf.data(), static_cast<int>(buf.size())) < 0) {
        return fail("engine.entity.toJson: invalid handle, or subtree exceeds 64 KiB");
    }
    return g_api->ret_string(g_api->js_ctx, buf.data());
}

GJsValue js_scene_pick(void*, GJsValue, int argc, const GJsValue* argv) {
    if (argc < 4) return fail("engine.scene.pick(sx, sy, width, height[, cameraId]) expects 4 arguments");
    double v[4] = {};
    for (int i = 0; i < 4; ++i) {
        if (!g_api->arg_number(g_api->js_ctx, argv[i], &v[i])) {
            return fail("engine.scene.pick: sx, sy, width and height must be numbers");
        }
    }
    GEntityHandle camera = 0;
    if (argc >= 5 && !read_handle(argv, argc, 4, &camera)) {
        return fail("engine.scene.pick: cameraId must be entity.id() or omitted");
    }
    const GEntityHandle hit = GScene_PickScreen(
        static_cast<float>(v[0]), static_cast<float>(v[1]),
        static_cast<int>(v[2]), static_cast<int>(v[3]), camera);
    return g_api->ret_number(g_api->js_ctx, static_cast<double>(hit));   // 0 = 未命中
}

} // namespace

GRYCE_EXTENSION_ENTRY {
    if (!api || api->version != GRYCE_EXTENSION_VERSION) return;
    g_api = api;
    api->add_function(api->js_ctx, "entity", "toJson", &js_entity_to_json, 1);
    api->add_function(api->js_ctx, "entity", "path", &js_entity_path, 1);
    api->add_function(api->js_ctx, "scene", "pick", &js_scene_pick, 4);
}
```

命名空间可以复用 JS 绑定已经建好的对象（`entity`、`scene`），所以脚本里看不出这是扩展补的：

```js
export function on_start() {
    // ...建窗、载场景、取相机...
    if (typeof engine.entity.path === 'function') {
        engine.log.info('camera path=' + engine.entity.path(camera.id()));
    }
    if (typeof engine.scene.pick === 'function') {
        const size = engine.window.size();
        const hit = engine.scene.pick(size[0] / 2, size[1] / 2, size[0], size[1], camera.id());
        engine.log.info('pick center -> ' + (hit > 0 ? engine.entity.path(hit) : '(miss)'));
    }
}
```

编译与运行：

```bash
# 调引擎能力 → 需要链接引擎导入库；头文件只需 <engine>/engine
g++ -shared -O2 test/demo_project/extension.cpp -I engine -L build -lGryceCore \
    -o test/demo_project/extension.dll

# 或直接用仓库样例目标（输出到构建目录）
cmake --build build --target GryceDemoExtension

# 宿主默认找 <project>/extension.dll；也可显式指定
build/GryceApp.exe --project test/demo_project --extension build/GryceDemoExtension.dll
```

扩展也可以完全不链接引擎（只经虚表注册纯计算函数），ABI 允许这样做。但那种写法等于把业务逻辑搬进 C++，只在「引擎确实没有、又必须用 C++」时才考虑；能映射到引擎 C API 的需求，一律走上面的适配形态。

## 4. ABI 参考

头文件：`engine/api/extension_api.h`。纯 C，`extern "C"` 导出，不包含 QuickJS 头，因此虚表本身与引擎的编译选项、C++ 运行时都无关。虚表只负责 JS 值通道；扩展要调引擎能力，另按第 1 节 include `engine/api/*.h` 并链接引擎导入库。

### 4.1 入口与注册函数

```c
#define GRYCE_EXTENSION_VERSION 1u

typedef void (*GryceExtensionRegisterFn)(GryceExtensionApi* api);
#define GRYCE_EXTENSION_ENTRY_NAME "gryce_register_extension"
```

`GRYCE_EXTENSION_ENTRY` 宏展开为带 `dllexport` / `visibility("default")` 的 `gryce_register_extension` 定义，扩展只要写 `GRYCE_EXTENSION_ENTRY { ... }` 即可，不要自己声明入口名。

### 4.2 `GJsValue`

```c
typedef struct GJsValue {
    int64_t payload;
    int64_t tag;
} GJsValue;
```

16 字节、按值传。它是 `JSValue` 的不透明镜像：桌面构建下 `JSValue` 是 16 字节的 `{union, tag}`，逐字节对应；Emscripten（`CONFIG_NAN_BOXING`）下 `JSValue` 是 8 字节单字，桥接层把 64 位字放进 `payload`。

**不要直接读写 `payload` / `tag`**。所有构造与读取都必须走虚表里的 `arg_*` / `ret_*`，这是跨构建配置（NAN_BOXING、值表示变更）唯一稳定的接口。

### 4.3 虚表字段

```c
typedef struct GryceExtensionApi {
    uint32_t version;
    uint32_t size;
    void* js_ctx;
    void (*add_function)(void* js_ctx, const char* ns, const char* name,
                         GJsNativeFn fn, int argc);
    GJsValue (*undefined)(void* js_ctx);
    GJsValue (*ret_number)(void* js_ctx, double v);
    GJsValue (*ret_bool)(void* js_ctx, int v);
    GJsValue (*ret_string)(void* js_ctx, const char* s);
    int (*arg_number)(void* js_ctx, GJsValue v, double* out);
    int (*arg_string)(void* js_ctx, GJsValue v, char* out, int cap);
    int (*arg_bool)(void* js_ctx, GJsValue v);
    GJsValue (*throw_error)(void* js_ctx, const char* msg);
    void (*log)(void* js_ctx, int level, const char* msg);
} GryceExtensionApi;
```

| 字段 | 语义 | 返回值 / 失败条件 |
| --- | --- | --- |
| `version` | ABI 版本，当前 `1`。扩展应校验 `api->version == GRYCE_EXTENSION_VERSION`，不匹配就直接返回（什么也不注册） | — |
| `size` | 本结构字节数。宿主未来追加字段时该值变大；扩展若用新字段，先判 `size >= offsetof(新字段) + sizeof(...)` 再访问 | — |
| `js_ctx` | 本次注册所属的 `JSContext`。**所有回调里要用的还是回调参数给的 `js_ctx`**，二者同值；不要缓存跨注册使用 | — |
| `add_function(ns, name, fn, argc)` | 在 `engine.<ns>` 上定义函数 `name`。`ns` 为 `NULL` 或 `""` 表示挂在 `engine` 本体（`engine.add()`）。`argc` 只设置 JS 函数的 `length` 属性，不限制实参个数 | 无返回；`ns`/`name`/`fn` 任一为空则静默忽略 |
| `undefined()` | JS `undefined` | `GJsValue` |
| `ret_number(v)` | 返回 JS number（float64） | `GJsValue` |
| `ret_bool(v)` | 返回 JS boolean，`v != 0` 为 `true` | `GJsValue` |
| `ret_string(s)` | 返回 JS 字符串（UTF-8，NUL 结尾）。`s` 为 `NULL` 时返回空串；内嵌 `\0` 会被截断 | `GJsValue` |
| `arg_number(v, out)` | 按 ECMAScript `ToNumber` 取数（`"2.5"` → 2.5，`true` → 1）。成功时写 `*out` | 成功 `1`，失败 `0`（`out` 不变）；`out` 为 `NULL` 直接 `0` |
| `arg_string(v, out, cap)` | 把值转成 UTF-8 串写入 `out`，最多 `cap-1` 字节 + 结尾 0。任意值都会试试 `ToString`（数字 → `"1.5"`，对象 → `"[object Object]"`） | 成功返回写入字节数（不含结尾 0，可能是截断后的长度）；失败 `-1`（`out` 空 / `cap<=0` / `ToString` 抛错） |
| `arg_bool(v)` | 按 JS 真值判断：`0` / `null` / `undefined` / `""` / `NaN` 为假，对象与数组为真 | `1` / `0` |
| `throw_error(msg)` | 抛 `TypeError`（`name: message`），并把异常值包装成 `GJsValue` | **必须直接 `return` 它**，不要继续做事 |
| `log(level, msg)` | 写引擎日志，前缀 `[ext]`。`level`：`0` info / `1` warn / `2` error / `3` debug，其他值按 info | 无 |

### 4.4 命名空间规则

`add_function` 的目标解析顺序（`extension_host.cpp` 的 `engine_namespace`）：

1. `ns` 为空 → `engine` 本体。
2. `engine[ns]` 是对象 → 直接用（因此可以往已有命名空间里加函数，例如 `engine.entity`、`engine.input`）。
3. `engine[ns]` 不存在或是非对象值 → 新建空对象并**覆盖**。

同名函数重复注册时后注册者胜出（扩展在应用绑定之后安装，所以扩展可以覆盖引擎同名接口，也应该避免这种撞名）。约定用扩展自己的命名空间（`engine.mytool` 之类）隔离；能对齐引擎既有命名空间的（如 `engine.entity.path`）优先对齐。

函数在 JS 侧是普通函数，`this` 为调用者对象；`fn` 的第一个参数 `this_val` 就是它，按值传入。

## 5. 值传递规则

| 方向 | 支持的类型 | 做法 |
| --- | --- | --- |
| 参数入 | 数字 | `arg_number` |
| 参数入 | 字符串 | `arg_string` + 固定缓冲 |
| 参数入 | 布尔 | `arg_bool` |
| 参数入 | 数组 / 对象 / 结构体 | 序列化成 JSON 字符串再 `arg_string`，C++ 侧用 `nlohmann::json`（或自带解析器）解析 |
| 返回值 | 数字 / 布尔 / 字符串 / `undefined` | `ret_number` / `ret_bool` / `ret_string` / `undefined` |
| 返回值 | 数组 / 对象 / 结构体 | `ret_string` 返回 JSON 文本，JS 侧 `JSON.parse` |
| 报错 | — | `throw_error` 并直接返回 |

**所有权**：`ret_*` 与 `throw_error` 产生的 `GJsValue` 由引擎接管（桥接层把它交回 QuickJS），扩展**不要**手工释放，也没有释放接口。参数 `argv` 指针与 `this_val` 只在本次调用期间有效，需要保留就自己拷贝内容。

**不要跨调用保存 `GJsValue`**：它不是引用计数句柄的稳定表示，跨调用持有会漏掉引用计数，或在热重载后指向已销毁的 context。

## 6. 生命周期与状态

| 事件 | 扩展侧可见的行为 |
| --- | --- |
| 宿主启动 | `dlopen` / `LoadLibrary`，注册入口调用一次（`main.js` 首次编译时） |
| `main.js` 热重载 | 重建 context → 注册入口**再次**调用；`api` 与 `js_ctx` 都是新的 |
| `main.js` 抛错停止派发 | 扩展函数不再被调用；库保持加载 |
| 进程退出 | 不调用任何卸载钩子；静态对象析构由加载器在进程结束时处理，不要依赖析构做清理 |

扩展的状态是**进程级**的，不随场景加载、`main.js` 重载重置。需要按次初始化的东西，在函数里做惰性初始化，或提供一个显式的 `engine.mytool.reset()` 让 JS 调。

与脚本组件的区别：组件脚本的 context 每个实体一个，扩展不参与；因此在组件脚本里访问扩展接口（如 `engine.mytool`）会得到 `undefined`。

## 7. 编译与分发

| 方式 | 命令 |
| --- | --- |
| MinGW / GCC（Linux 同构） | `g++ -shared -O2 extension.cpp -I <engine> -o extension.dll` |
| MSVC | `cl /nologo /LD /O2 /I <engine> extension.cpp /Fe:extension.dll` |
| 仓库样例目标 | `cmake --build build --target GryceDemoExtension` |

要点：

- 只需要 `<engine>/engine` 一个 include 路径，不需要链接 `GryceCore`，也不需要 QuickJS 头。
- 跨编译器：跨界只有 C 类型与 POD 按值传递，没有 C++ 类、STL 容器、异常或 RTTI，因此不存在 C++ ABI 耦合；同一平台同一架构下，MSVC 与 MinGW 各自编出的扩展都能被宿主加载。仓库当前只验证过 MinGW 工具链，混用编译器时如遇异常先查调用约定与聚合体传参。
- 32/64 位必须与宿主一致；Debug/Release 混用没有问题（不跨越 C++ 运行库边界），但不要跨库传递由 `new` 分配的内存（谁分配谁释放）。
- 分发时放在 `<project>/extension.dll`，打包器会随工程目录带上；宿主用绝对路径加载，扩展与宿主 DLL 的相对位置不影响查找。

## 8. 调试

- 扩展里的 `g_api->log(g_api->js_ctx, 0, "msg")` 直接进引擎日志，前缀 `[ext]`，是最省事的通道。
- JS 侧看到的错误来自 `throw_error`，类型是 `TypeError`，消息原样出现在栈里；参数校验写在函数开头最省排查时间。
- 注册阶段的问题（入口名不对、版本不匹配、`add_function` 传空）不会抛到 JS，只在加载失败时有一条 `[app] extension load failed`。注册后 `engine.<ns>.<name>` 仍是 `undefined` 时，先确认版本校验没有提前 `return`。
- 崩溃在扩展函数内时，日志里最后一条 `[ext]` 或调用点附近的脚本行号即为线索；常见原因是缓存了旧 `js_ctx`（见第 2 节）或在回调里保存了 `GJsValue`。

## 9. 限制

- **Web 构建不支持**：wasm 侧没有动态库加载，`load_extension_library` 直接返回错误（`extensions are not supported on the Web build`），`GryceApp` 本身也不参与 Web 构建。
- 只暴露标量通道，结构体走 JSON 序列化，逐帧高频调用大对象的开销要自己评估。
- 不能注册模块、不能扩展 JS 语法、不能拦截已有函数（同名只能覆盖，不能包装调用原实现）。
- 虚表本身不含引擎句柄操作：扩展拿到的只是 JS 值；要访问实体/场景，走链接进来的 C API（`GEntity_*` / `GScene_*`），不要试图把引擎 C++ 对象塞进 `GJsValue`。
- 注册表只追加不复用：每次 `main.js` 重载都会为每个 `add_function` 追加一条内部条目。功能无影响，但反复热重载是缓慢增长；需要频繁重载的场合把注册收敛到少数函数。

## 10. 排错表

| 现象 | 原因 | 处置 |
| --- | --- | --- |
| 扩展接口（如 `engine.mytool`）是 `undefined` | 库没加载（路径不对 / 缺 `--extension`）、入口名不是 `gryce_register_extension`、版本校验提前返回 | 看 `[app] extension load failed` 与注册日志；确认导出名 |
| 加载时报入口找不到 | 用了 C++ 名字修饰（未加 `extern "C"`）或没写 `GRYCE_EXTENSION_ENTRY` | 用宏定义入口 |
| 首次调用正常，改 `main.js` 后崩溃 | 缓存了旧 `api` / `js_ctx` 且未在注册入口刷新 | 每次注册覆盖缓存指针 |
| 调用后进程退出码 139 / 0xC0000005 | 保存了 `GJsValue` 或 `argv` 指针跨调用使用；或 `api` 指针来自栈上临时表 | 只按值拷贝内容；不要自己构造 `GryceExtensionApi` |
| `arg_string` 拿到的是 `"[object Object]"` | 传了对象而不是字符串 | 用 `JSON.stringify` 后再传，或改契约 |
| `arg_number` 返回 0 但看起来是数字 | 传了 `undefined` / `null`，或对象无法 `ToNumber` | 先判 `typeof`，缺省值在 JS 侧补 |
| 返回的字符串被截断/乱码 | 缓冲不足被截断在 UTF-8 多字节中间；或 `ret_string` 传了非 UTF-8 | 加大缓冲；统一用 UTF-8 |
| 扩展里读不到 `engine.app.*` | 扩展只能注册函数，看不到 JS 侧对象图 | 需要的数据由 `main.js` 当参数传进来；引擎侧状态直接调 C API |
| 链接报 `undefined reference to GEntity_*` | 没链接引擎导入库 | `target_link_libraries(<ext> PRIVATE GryceCore)`，或用 `-L <build> -lGryceCore` |
| 加载后调用崩溃，但 `nm` 里符号都在 | 混用了不同引擎构建的导入库 | 扩展与宿主必须来自同一次引擎构建 |
