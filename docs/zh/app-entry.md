# 应用入口（main.js）

本文是**入口脚本**形态的完整用法：固定宿主 `GryceApp.exe` + 项目入口 `main.js`，涵盖启动顺序、钩子、全部入口 API、命令行与 `project.data` 的优先级、以及常见限制。

相关文档：

| 主题 | 文档 |
| --- | --- |
| 组件级脚本（`ScriptComponent`） | [scripting.md](scripting.md) |
| 原生扩展完整 ABI 与用法 | [extension.md](extension.md) |
| 沙箱、执行预算、值约定（两种形态共用） | [scripting.md](scripting.md) 第 6、7 节 |

## 1. 形态

| 角色 | 位置 | 是否随项目变化 |
| --- | --- | --- |
| 宿主模板 | `engine/app/app_main.cpp`（三行 `run(argc, argv)`） | 否，全项目共用 |
| 宿主实现 | `engine/app/app_host.cpp`（参数、初始化、帧循环、截图） | 否 |
| 入口脚本 | `<project>/main.js`，默认 `res:/main.js` | 是，项目逻辑全在这里 |
| 原生扩展 | `<project>/extension.cpp` → `extension.dll` | 可选 |

因此项目目录里不再需要 `main.cpp`：窗口、场景、相机、退出条件都由 `main.js` 调 `engine.*` 决定。

与 `ScriptComponent` 的分工：组件挂在实体上、随场景生命周期启停；入口脚本属于进程本身，一个进程一个，负责"整个应用"。

## 2. 启动顺序

1. 解析命令行（`--project` / `--entry` / `--api` / `--w` / `--h` / `--frames` / `--shot` / `--extension` / `--fps` / `--debug-ui`），读 `<project>/project.data`。
2. `GCore_Init`（`GCore_SetAutoLoadMainScene(false)`：不自动载主场景），压入 `ECMD_PLAY_MODE`，使组件 `on_update`、动画、物理在第一帧就生效。
3. 创建 `ScriptApplication`，注册 `on_context_created` 回调：安装应用绑定 → 加载扩展动态库（若存在）。
4. 读取并求值 `main.js`（ES module 语义），提取 `on_start` / `on_update` / `on_destroy`，然后派发 `on_start()`。
5. 每帧循环：

   ```text
   GWindow_PollEvents()          // 仅当窗口已存在
   GInput_SyncToCore()           // 平台输入 -> 命令队列
   GCore_BeginFrame(dt)          // 处理命令、驱动 World
   script.tick(dt)               // 热重载轮询 + on_update(dt) + 微任务
   GRender_BeginFrame/RenderWorld/RenderGizmo/EndFrame   // 仅当渲染器已初始化
   GCore_EndFrame()
   ```

   没有 `GInput_SyncToCore()` 这一步，`engine.input.*` 永远读不到输入——它负责把平台层的按键/鼠标状态推成 `ECMD_INPUT_*` 命令，`GCore_BeginFrame` 再消费成 `g_input_state`。

6. 退出时派发 `on_destroy()`，然后按 **先拆场景（`GCore_Shutdown`）、再拆渲染器（`GRender_Shutdown`）、最后销毁窗口** 的顺序收尾。顺序反了会对已释放的渲染上下文做析构。

`dt` 由 `std::chrono::steady_clock` 计算并夹到 `[0.0005, 0.1]` 秒，避免断点或长卡顿后出现巨型 dt。

## 3. 生命周期钩子

模块导出的函数即钩子，三个都可省略：

| 钩子 | 签名 | 时机 |
| --- | --- | --- |
| `on_start()` | `export function on_start()` | 入口装载成功后一次；热重载成功后重放 |
| `on_update(dt)` | `export function on_update(dt)`，`dt` 为秒 | 每帧，在 `GCore_BeginFrame` 之后、渲染之前；此时 `engine.input.*` 已是本帧输入 |
| `on_destroy()` | `export function on_destroy()` | 进程退出前一次 |

`this` 是一个空对象，入口脚本没有实体宿主（因此 `engine.entity.name()` 这类"宿主实体"接口在这里会抛 `no host entity`）。钩子抛错会被记录（`name: message` + stack）并停止派发，热重载修好文件后自动恢复。

顶层代码在每次编译时执行（含热重载），适合放常量、辅助函数与扩展能力探测；需要引擎状态的初始化写进 `on_start`。

## 4. 入口 API

入口脚本 = 基础绑定（`engine.log` / `time` / `input` / `entity` 的宿主实体视角，见 [scripting.md](scripting.md) 第 5 节）+ 应用绑定（下表）。应用绑定由 `install_app_bindings` 安装，多出窗口、场景、实体查询与应用控制。

### 4.1 `engine.window`

| 签名 | 返回 | 说明 |
| --- | --- | --- |
| `engine.window.create(opts)` | `bool` | 建窗口并初始化渲染器。成功 `true`；`width`/`height` 非正抛 `TypeError`；建窗或初始化渲染器失败返回 `false` 并记 error 日志 |
| `engine.window.close()` | `undefined` | 先 `GRender_Shutdown` 再 `GWindow_Destroy`；可重复调用 |
| `engine.window.valid()` | `bool` | 窗口是否有效 |
| `engine.window.size()` | `number[2]` | `[w, h]`，取 framebuffer 尺寸（高 DPI 下与逻辑尺寸不同） |

`opts` 字段：

| 字段 | 类型 | 默认 | 说明 |
| --- | --- | --- | --- |
| `title` | string | `"Gryce"` | 窗口标题 |
| `width` | number | `1280` | 客户区宽（像素），必须 > 0 |
| `height` | number | `720` | 客户区高（像素），必须 > 0 |
| `api` | string | 跟随 `--api` → `project.data.render_api` → `opengl` | `opengl` / `vulkan` / `dx12` / `webgl`；非法值返回 `false` |
| `vsync` | bool | `true` | 垂直同步 |
| `resizable` | bool | `true` | 当前版本仅解析、未生效（窗口模式固定 windowed） |

语义要点：

- **幂等**：窗口已存在时直接返回 `true`，不会重建，也不会应用本次传入的尺寸/后端。要换后端只能先 `close()` 再 `create()`。
- 渲染后端的优先级链是 `opts.api` > `--api` > `project.data.render_api` > `opengl`。
- 建窗时还会应用 `project.data` 里的渲染参数（HDR、色调映射、曝光、阴影、环境光、IBL、SSAO/SSR/SSIL、PCSS）。
- 不调 `create()` 也能跑：纯逻辑脚本（CI 里的数值验证、批量数据处理）可以完全无窗口运行；此时 `engine.app.has_renderer()` 为 `false`，`engine.app.screenshot()` 返回 `false`。

### 4.2 `engine.scene`

| 签名 | 返回 | 说明 |
| --- | --- | --- |
| `engine.scene.load(path)` | `bool` | **同步**载入场景，替换当前场景。失败返回 `false` 并记 error 日志。`path` 支持 `res:/` 与绝对路径 |
| `engine.scene.current()` | `string` | 当前场景路径；未载入时为空串 |

载入会清掉旧场景的实体：之前拿到的实体代理全部失效（`valid()` 返回 `false`，其余方法抛 `stale handle` 或操作失败）。载入后重新 `find`。

### 4.3 `engine.entity`（全量枚举）

| 签名 | 返回 | 说明 |
| --- | --- | --- |
| `engine.entity.count()` | `number` | 实体总数（含根实体） |
| `engine.entity.find(name)` | 实体代理 \| `null` | 按**精确名字**取第一个匹配；未命中 `null` |
| `engine.entity.all()` | 实体代理数组 | 全部实体，顺序即句柄表顺序 |
| `engine.entity.spawn(json)` | 实体代理 | 按 `GEntity_ImportJson` 的格式从 JSON 创建实体子树（挂在场景根，生成新 UUID / EntityID）；失败抛 `import failed` |
| `engine.entity.destroy(e)` | `undefined` | 销毁实体（代理或裸句柄都接受）；走命令缓冲，下一帧 `GCore_BeginFrame` 处理 |

枚举口径是 `EntityHandleMap::all_handles()`，**不是** `GEntity_GetCount/GetAt`——后者只统计有父节点的实体，场景里的根实体（相机、灯光、模型）在那里不可见，用它会得到 0。

实体代理对象（`find` / `all` / `spawn` 的返回值）方法：

| 方法 | 返回 | 说明 |
| --- | --- | --- |
| `e.id()` | `number` | 实体句柄 |
| `e.valid()` | `bool` | 句柄是否仍指向存活实体 |
| `e.name()` | `string` | 名字 |
| `e.set_name(s)` | `undefined` | 改名（命令缓冲，下一帧生效） |
| `e.position()` | `number[3]` | 本地位置 |
| `e.set_position(x,y,z)` / `([x,y,z])` | `undefined` | 写本地位置 |
| `e.rotation()` | `number[4]` | 本地旋转四元数 `[x,y,z,w]` |
| `e.set_rotation(x,y,z,w)` / `([x,y,z,w])` | `undefined` | 写本地旋转 |
| `e.scale()` | `number[3]` | 本地缩放 |
| `e.set_scale(x,y,z)` / `([x,y,z])` | `undefined` | 写本地缩放 |
| `e.translate(dx,dy,dz)` / `([dx,dy,dz])` | `undefined` | 位置累加 |
| `e.enabled()` | `bool` | 实体启用位 |
| `e.set_enabled(bool)` | `undefined` | 写启用位 |
| `e.destroy()` | `undefined` | 销毁该实体（命令缓冲） |

句柄失效后：`valid()` 返回 `false`；读写属性的方法抛 `TypeError`（消息含 `stale handle` 或 `... failed`）。因此长期持有的代理每次用前判 `valid()` 是最省事的写法。

### 4.4 `engine.app`

| 签名 | 返回 | 说明 |
| --- | --- | --- |
| `engine.app.quit()` | `undefined` | 请求退出：当前帧跑完后跳出循环，派发 `on_destroy()` 并正常收尾 |
| `engine.app.fps()` | `number` | 渲染器统计的 FPS；无渲染器时为 `0` |
| `engine.app.has_window()` | `bool` | 窗口是否有效（同 `engine.window.valid()`） |
| `engine.app.has_renderer()` | `bool` | 渲染器是否已初始化 |
| `engine.app.screenshot(path)` | `bool` | 请求截图。无渲染器或请求失败返回 `false` |

`engine.app.screenshot` 是**异步**的：渲染器在后续帧才真正写盘。宿主内部把"待 flush 帧数"记为 4，因此：

- 截图后继续跑帧即可，文件在 4 帧内出现；
- 截图后立刻 `quit()` 不会丢图——退出条件会等 flush 计数归零。

路径按进程当前工作目录解析，建议传绝对路径。

### 4.5 输入

| 签名 | 返回 | 说明 |
| --- | --- | --- |
| `engine.input.set_mouse_locked(bool)` | `undefined` | 指针锁定。锁定后鼠标不再受窗口边界限制，`engine.input.mouse_delta()` 持续给出位移，适合 FPS 视角 |

其余 `engine.input.*`（`key_down` / `key_up` / `mouse_x` / `mouse_y` / `mouse_delta` / `mouse_button` / `mouse_locked`）与组件脚本一致，键码为 GLFW 键码，详见 [scripting.md](scripting.md) 5.3 节。

## 5. `project.data` 与命令行

装载入口前读取 `<project>/project.data`（JSON，与 `GryceRenderSmoke` 同口径）。宿主实际用到的字段：

| 字段 | 默认 | 说明 |
| --- | --- | --- |
| `name` / `window.title` | `"Gryce"` | 窗口标题（`window.title` 优先） |
| `window.width` / `window.height` | `1280` / `720` | 建窗默认尺寸 |
| `render_api` | `"opengl"` | 默认后端 |
| `hdr` / `tone_map_mode` / `exposure` | `true` / `1` / `1.0` | 色调映射 |
| `shadow_enabled` / `shadow_map_size` | `true` / `2048` | 阴影 |
| `ambient_r` / `ambient_g` / `ambient_b` | `0.2` / `0.22` / `0.26` | 环境光 |
| `ibl_intensity` / `default_environment` | `1.0` / `false` | 环境贴图 |
| `ssao_enabled` / `ssr_enabled` / `ssil_enabled` | `false` | 屏幕空间效果开关 |
| `ssr_max_steps` / `ssr_max_roughness` / `ssr_thickness` / `ssr_bilateral_filter` / `ssr_env_fallback` / `ssr_resolution_scale` | `64` / `0.6` / `0.1` / `0.5` / `1.0` / `1.0` | SSR 参数 |
| `pcss_enabled` / `pcss_light_size` / `pcss_max_radius` | `false` / `0.06` / `12.0` | 软阴影 |

优先级：命令行显式值 > `project.data` > 内置默认。`project.data` 缺失或解析失败时全部走内置默认，只打一条 `[app] project.data parse failed`。

## 6. 命令行

| 参数 | 说明 |
| --- | --- |
| `--project <dir>` | 项目根（默认进程当前目录；`res:/` 由此解析，也是 `project.data` 与默认扩展的查找位置） |
| `--entry <path>` | 入口脚本（默认 `res:/main.js`） |
| `--api <name>` | `opengl` / `vulkan` / `dx12`，优先于 `project.data.render_api` |
| `--w` / `--h <px>` | 窗口尺寸覆盖（注意是**两个独立参数**，`--w 1920 --h 1080`） |
| `--frames <N>` | 跑满 N 帧后退出；`0`（默认）表示不限帧 |
| `--shot <png>` | 跑满帧数后截图再退出；隐含 `--frames 60`，并等写盘 flush 完成 |
| `--extension <path>` | 指定扩展动态库，覆盖默认的 `<project>/extension.dll` |
| `--no-extension` | 关闭默认路径查找（显式 `--extension` 仍生效） |
| `--fps` | 左上角 FPS 叠加（默认关） |
| `--debug-ui` | 渲染器 ImGui 调试面板（默认关；运行时默认不挂，`GryceRenderSmoke` 才是冒烟台） |
| `--help` / `-h` | 打印用法并退出 |

未知参数会打印用法并以退出码 1 结束。

## 7. 原生扩展

扩展是可选能力，用法全文见 [extension.md](extension.md)。要点：

- 动态库导出 `gryce_register_extension`，只依赖 `engine/api/extension_api.h`（纯 C），不链接引擎库。
- 加载发生在 `main.js` 求值之前，所以顶层代码即可调用扩展接口。
- `engine.<命名空间>.<函数>`；值通道只有标量，结构体走 JSON 字符串。
- 只在桌面构建可用；Web 构建没有动态库加载。
- `main.js` 里按 `typeof` 探测，缺扩展时保持可运行。

## 8. 示例拆解

`test/demo_project/main.js` 的骨架，可直接作为新项目起点：

```js
export function on_start() {
    // 可选扩展：先探测再调用
    if (engine.game && typeof engine.game.version === 'function') {
        engine.log.info('ext: ' + engine.game.version());
    }

    if (!engine.window.create({ title: 'Gryce Demo', width: 1280, height: 720 })) {
        engine.log.error('window.create failed');
        return;
    }
    if (!engine.scene.load('res:/scenes/main.gesc')) {
        engine.log.error('scene load failed');
        return;
    }

    camera = engine.entity.find('MainCamera');
    if (!camera) {
        engine.log.error('MainCamera not found');
        return;
    }

    // 从场景里的相机朝向反解 yaw/pitch，接管视角时针头不跳
    const q = camera.rotation();
    const f = rotate(q, [0, 0, -1]);
    pitch = Math.asin(clamp(f[1], -1, 1)) * 180 / Math.PI;
    yaw = Math.atan2(-f[0], -f[2]) * 180 / Math.PI;
    position = camera.position();

    engine.input.set_mouse_locked(true);
}

export function on_update(dt) {
    if (engine.input.key_down(256 /* ESC */)) {   // GLFW 键码
        engine.app.quit();
        return;
    }
    if (engine.input.mouse_locked()) {
        const d = engine.input.mouse_delta();
        yaw -= d[0] * 0.12;
        pitch = clamp(pitch - d[1] * 0.12, -89, 89);
    }
    // ... WASD 位移，最后写回相机
    camera.set_position(position[0], position[1], position[2]);
    camera.set_rotation(view_quat());
}

export function on_destroy() {
    engine.input.set_mouse_locked(false);
}
```

三个值得照抄的实践：

- **失败早退**：建窗、载场景、找相机任一步失败就 `return`，否则后续每帧都在空代理上调用。
- **朝向反解**：不要假设相机初始朝向，从 `rotation()` 反解 yaw/pitch，避免接管瞬间跳变。
- **锁定判据**：只有 `mouse_locked()` 为真时才消费 `mouse_delta()`，否则 Alt-Tab 回来会甩视角。

## 9. 验证

```bash
# 三后端各跑一次（建窗、载场景、接管相机，60 帧后截图）
build/GryceApp.exe --project test/demo_project --frames 60 --shot shot_gl.png   --api opengl
build/GryceApp.exe --project test/demo_project --frames 60 --shot shot_vk.png   --api vulkan
build/GryceApp.exe --project test/demo_project --frames 60 --shot shot_dx12.png --api dx12

# 无窗口跑纯逻辑（CI 里验证脚本数值行为最省事）
build/GryceApp.exe --project <dir> --entry res:/logic.js --frames 30
```

`ctest -R script_app_entry` 覆盖入口脚本的钩子派发、热重载换模块与扩展 `add_function` 注册，不依赖 GPU 与窗口。

## 10. 限制

- **Web 构建不提供 `GryceApp`**：wasm 侧仍是 `GryceRenderSmoke` 的常驻循环 + 预载 payload，扩展也不可用。
- 一个进程一个入口脚本；多场景切换用 `engine.scene.load`，不要再加载第二个入口。
- 热重载判据是 mtime + 文件大小；同一时间粒度内、长度不变的改动无法被发现（与 `ScriptComponent` 同口径）。
- `engine.window.create` 只能成功一次；要换后端必须先 `close()`。
- 入口脚本没有实体宿主：`engine.entity.name()` 等"宿主实体"接口抛 `no host entity`，实体访问一律经 `find` / `all` / `spawn` 的代理。
- `engine` 对象上不存在 `physics` / `audio` / `asset` 等命名空间；未实现的接口不会以空函数形式出现，`typeof` 探测是可靠判据。
