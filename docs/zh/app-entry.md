# 应用入口（main.js）

本文描述"项目只写 JavaScript"的运行形态：固定宿主 `GryceApp.exe` + 项目入口 `main.js`，以及用 `extension.cpp` 扩展原生接口的方式。组件级脚本（`ScriptComponent`）见 [scripting.md](scripting.md)。

## 1. 形态

| 角色 | 位置 | 是否随项目变化 |
| --- | --- | --- |
| 宿主模板 | `engine/app/app_main.cpp`（几行 `run(argc, argv)`） | 否，全项目共用 |
| 宿主实现 | `engine/app/app_host.cpp`（参数、初始化、帧循环、截图） | 否 |
| 入口脚本 | `<project>/main.js`，默认 `res:/main.js` | 是，项目逻辑全在这里 |
| 原生扩展 | `<project>/extension.cpp` → `extension.dll` | 可选 |

因此项目目录里不再需要 `main.cpp`：窗口、场景、相机、退出条件都由 `main.js` 调 `engine.*` 决定。

与 `ScriptComponent` 的分工：组件挂在实体上、随场景生命周期启停；入口脚本属于进程本身，一个进程一个，负责"整个应用"。

## 2. 启动顺序

1. 解析 `--project` / `--entry` / `--api` 等参数，读 `project.data`（窗口尺寸、渲染参数默认值）。
2. `GCore_Init`（`auto_load_main_scene = false`），压入 `ECMD_PLAY_MODE`：组件 `on_update`、动画、物理在第一帧就生效。
3. 若存在扩展动态库则加载（在 `main.js` 求值之前）。
4. 求值 `main.js`，派发 `on_start()`。
5. 每帧：
   - `GWindow_PollEvents()` + `GInput_SyncToCore()`（没有这一步 `engine.input.*` 永远读不到输入）；
   - `GCore_BeginFrame(dt)`（处理命令、驱动 World）；
   - `on_update(dt)`；
   - 建过窗口时提交一帧渲染；
   - `GCore_EndFrame()`。
6. 退出时派发 `on_destroy()`，然后按 **先拆场景、再拆渲染器、最后销毁窗口** 的顺序收尾（顺序反了会对已释放的渲染上下文做析构，见 `renderer_smoke.cpp` 末尾的注释）。

## 3. 生命周期钩子

模块导出的函数即钩子，三个都可省略：

| 钩子 | 时机 |
| --- | --- |
| `on_start()` | 入口装载成功后一次；热重载成功后重放 |
| `on_update(dt)` | 每帧，`dt` 为秒；此时 `engine.input.*` 已是本帧输入 |
| `on_destroy()` | 进程退出前一次 |

`this` 是一个空对象，入口脚本没有实体宿主。抛错会被记录（`name: message` + stack）并停止派发，热重载修好文件后自动恢复。

## 4. 脚本侧 API

在组件脚本已有的 `engine.log` / `engine.time` / `engine.input` / 宿主实体接口之外，入口脚本额外获得：

| API | 说明 |
| --- | --- |
| `engine.window.create({title,width,height,api,vsync,resizable})` | 建窗口并初始化渲染器；`api` 省略时跟随 `project.data`，可传 `opengl`/`vulkan`/`dx12` |
| `engine.window.close()` / `valid()` / `size()` | 关窗 / 是否有效 / `[w,h]` |
| `engine.input.set_mouse_locked(bool)` | 指针锁定（FPS 视角） |
| `engine.scene.load(path)` / `current()` | 载场景（同步）/ 当前场景路径 |
| `engine.entity.count()` | 实体总数（含根实体） |
| `engine.entity.find(name)` | 按名字取实体代理，未命中返回 `null` |
| `engine.entity.all()` | 全部实体代理数组 |
| `engine.entity.spawn(json)` / `destroy(e)` | 用 `GEntity_ExportJson` 同格式的 JSON 生成实体 / 销毁 |
| `engine.app.quit()` / `fps()` / `has_window()` / `has_renderer()` | 请求退出 / 帧率 / 状态查询 |
| `engine.app.screenshot(path)` | 请求截图（渲染线程在后续帧写盘，宿主退出前会等 4 帧） |

实体代理对象（`find` / `all` / `spawn` 的返回值）：

```js
const cam = engine.entity.find('MainCamera');
cam.id();  cam.valid();  cam.name();  cam.set_name('Cam');
cam.position();            // [x,y,z]
cam.set_position(0, 34, 46);
cam.rotation();            // [x,y,z,w]
cam.set_rotation(qx, qy, qz, qw);
cam.scale();  cam.set_scale(1, 1, 1);
cam.translate(dx, dy, dz);
cam.enabled();  cam.set_enabled(true);
cam.destroy();
```

`engine.entity.count/find/all` 走实体句柄表全量枚举（`EntityHandleMap::all_handles`），不是 `GEntity_GetCount/GetAt`——后者只统计有父节点的实体，场景里的根实体在那里不可见。

## 5. 原生扩展

扩展是一个动态库，导出 `gryce_register_extension`，由宿主在 `main.js` 求值前加载。ABI 只依赖 `engine/api/extension_api.h`（纯 C，不含 QuickJS 头，也不链接引擎库），因此扩展与引擎的 C++ ABI / 编译选项解耦。

```cpp
#include "api/extension_api.h"

static GryceExtensionApi* g_api = nullptr;

static GJsValue js_add(void*, GJsValue, int argc, const GJsValue* argv) {
    double a = 0.0, b = 0.0;
    if (argc < 2 || !g_api->arg_number(g_api->js_ctx, argv[0], &a) ||
        !g_api->arg_number(g_api->js_ctx, argv[1], &b)) {
        return g_api->throw_error(g_api->js_ctx, "engine.game.add(a,b)");
    }
    return g_api->ret_number(g_api->js_ctx, a + b);
}

GRYCE_EXTENSION_ENTRY {
    if (!api || api->version != GRYCE_EXTENSION_VERSION) return;
    g_api = api;   // 指针在进程内一直有效
    api->add_function(api->js_ctx, "game", "add", &js_add, 2);
}
```

JS 侧直接 `engine.game.add(1, 2)`。要点：

- `api` 指针由宿主持有、进程内一直有效，可以缓存成静态变量；不要保存 `js_ctx` 之外的 JS 值。
- 值只有标量通道（`arg_number` / `arg_string` / `arg_bool` 与对应 `ret_*`）；结构体走 JSON 字符串。
- 注册时机是每次新建 context（含 `main.js` 热重载）时，扩展自身的静态状态不重置。
- 扩展只在桌面构建可用，Web 构建（wasm）没有动态库加载。

编译方式二选一：

```bash
# 独立编译（只给头文件路径，不链接引擎）
g++ -shared -O2 test/demo_project/extension.cpp -I engine -o extension.dll

# 仓库内的样例目标（输出到构建目录）
cmake --build build --target GryceDemoExtension
```

默认查找 `<project>/extension.dll`（Linux 为 `extension.so`），也可用 `--extension <path>` 指定，`--no-extension` 关闭。仓库样例 `test/demo_project/extension.cpp` 提供 `engine.game.version/add/greet`，`main.js` 里按 `typeof` 探测使用。

## 6. 命令行

| 参数 | 说明 |
| --- | --- |
| `--project <dir>` | 项目根（默认当前目录，`res:/` 由它解析） |
| `--entry <path>` | 入口脚本（默认 `res:/main.js`） |
| `--api <name>` | `opengl` / `vulkan` / `dx12`，优先于 `project.data` |
| `--w` / `--h <px>` | 窗口尺寸覆盖 |
| `--frames <N>` | 跑满 N 帧退出；`0` 表示不限 |
| `--shot <png>` | 跑满帧数后截图再退出（隐含 `--frames 60`） |
| `--extension <path>` / `--no-extension` | 指定 / 禁用扩展 |
| `--fps` | 左上角 FPS 叠加（默认关） |
| `--debug-ui` | 渲染器 ImGui 调试面板（默认关） |

## 7. 验证

```bash
# 三后端各跑一次（main.js 建窗、载场景、接管相机，60 帧后截图）
build/GryceApp.exe --project test/demo_project --frames 60 --shot shot_gl.png   --api opengl
build/GryceApp.exe --project test/demo_project --frames 60 --shot shot_vk.png   --api vulkan
build/GryceApp.exe --project test/demo_project --frames 60 --shot shot_dx12.png --api dx12
```

`ctest -R script_app_entry` 覆盖入口脚本的钩子派发、热重载换模块与扩展 `add_function`（不依赖 GPU / 窗口）。

## 8. 限制

- Web 构建不提供 `GryceApp`：wasm 侧仍是 `GryceRenderSmoke` 的常驻循环 + 预载 payload，扩展也不可用。
- 热重载判据是 mtime + 文件大小；同一时间粒度内、长度不变的改动无法被发现（与 `ScriptComponent` 同口径）。
- 一个进程一个入口脚本；多场景切换请用 `engine.scene.load`，不要试图再加载第二个入口。
- `engine.window.create` 只能在有渲染器时成功一次；重复调用返回 `true` 但不重建窗口。
