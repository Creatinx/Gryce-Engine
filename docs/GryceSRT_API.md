# GryceSRT 脚本 API 与游戏打包手册

> GryceSRT = GryceEngine Script Runtime（Core 内嵌 QuickJS）。
> 脚本通过 `Script` 组件挂到实体上，播放/打包运行时由 ScriptSystem 驱动。
> 玩法逻辑（角色控制、AI、关卡流程等）用 **ES Module JavaScript** 编写；
> Lua 运行时已完全移除，存量 Lua 脚本需手工重写（见 [迁移指南](./MIGRATION_GUIDE.md)）。

## 1. 脚本生命周期

在 `.js` 中定义以下可选导出函数：

```js
export const props = { speed: 1.0, label: "hello" };  // 暴露属性（Inspector 可编辑、随场景保存）

export function on_start() {}          // 实体创建/组件挂载、或播放开始时调用一次
export function on_update(dt) {}       // 每帧调用，dt 为帧时间（秒）
export function on_destroy() {}        // 组件移除/场景关闭/重载前调用
```

脚本顶层代码在加载时执行一次。每个模块拥有独立作用域，全局变量互不污染。

## 2. `engine.*` API

完整签名与示例见 [脚本 API 参考](./SCRIPT_API_REFERENCE.md)。摘要：

| 分组 | 函数 | 说明 |
|---|---|---|
| 运行时 | `engine.version()` | 返回运行时版本字符串 |
| 实体 | `engine.self()` | 当前脚本所属实体句柄（0 表示无） |
| | `engine.entity.get_name(h)` | 实体名 |
| | `engine.entity.find(name)` | 按名字查找实体，返回句柄（0 = 未找到） |
| | `engine.entity.find_all(prefix)` | 查找名字为 `prefix` 或 `prefix<数字>` 的全部实体，返回句柄数组 |
| | `engine.entity.create(name, parent?)` | 在当前场景创建实体（脚本遍历期间安全） |
| | `engine.entity.destroy(h)` | 延迟销毁实体（本帧脚本遍历结束后生效） |
| | `engine.entity.aabb(h)` | 实体 AABB `{x, y, z, w, h, d}` |
| | `engine.entity.get_transform(h)` | 返回 `{position, rotation, scale}`（各为 `{x,y,z}`） |
| | `engine.entity.set_transform(h, transform)` | 写回变换 |
| 组件 | `engine.component.has(h?, type)` | 实体是否有该类型组件（h 省略用 `engine.self()`） |
| | `engine.component.get(h?, type)` | 读取组件字段 `{prop: value}` |
| | `engine.component.set(h?, type, props)` | 写组件字段 |
| 状态 | `engine.state.get/set/has(key)` | 跨实体、跨场景共享的游戏状态 |
| 输入 | `engine.input.key_down(key)` | 按键是否按住（GLFW 键码，如 W=87） |
| | `engine.input.mouse_pos()` | 返回鼠标 x, y |
| | `engine.input.mouse_down(button)` | 鼠标键是否按住（0=左，1=右，2=中） |
| 时间 | `engine.time.delta()` / `engine.time.elapsed()` | 帧时间 / 累计运行时间（秒） |
| 日志 | `engine.log.info/warn/error(msg)` | 输出到引擎日志/编辑器控制台 |
| 场景 | `engine.scene.load(path)` | 切换到指定场景（`res:/...`） |
| | `engine.scene.current()` | 当前场景的 `res:/` 路径 |
| 音频 | `engine.audio.play_on(h)` | 播放实体上 `AudioSource` 组件 |
| 特效 | `engine.fx.burst(h)` | 实体上 `ParticleEmitter2D` 爆发一次 |
| JSON | `engine.json.read(path)` | 读取项目内 JSON |
| 物理 | `engine.physics.set_gravity(x, y)` / `get_gravity()` | 设置/读取当前 2D 物理世界重力 |

## 2.1 主场景（Main Scene）

Core 增加"主场景"概念：游戏启动时自动进入主场景。

- 主场景配置：`project.gproj`（唯一配置）的 `"main_scene"` 字段（如 `"main_scene":"res:/scenes/main.gesc"`），缺省为 `res:/scenes/main.gesc`。
- 游戏入口（GryceGame 模板）在 `GCore_Init` 前调用 `GCore_SetAutoLoadMainScene(true)`，Core 初始化完成后自动加载主场景；命令行 `--scene <path>` 可覆盖。
- 编辑器默认加载主场景：打开项目后编辑器自动加载 `main_scene` 指定的场景（缺省
  `res:/scenes/main.gesc`），新建项目会立即把脚手架生成的 `main.gesc` 保存为主场景。
- 运行中切换场景：`engine.scene.load("res:/scenes/xxx.gesc")`。

示例（在脚本里按下某个键切换到另一个场景）：

```js
export function on_update(dt) {
    if (engine.input.key_down(32)) {          // Space
        engine.scene.load("res:/scenes/level2.gesc");
    }
}
```

## 2.2 数学库（`math.*` 扩展）

GryceSRT 在标准 `Math` 之外补充了常用游戏数学函数（全部已注册，见
[脚本 API 参考](./SCRIPT_API_REFERENCE.md)）：

| 函数 | 说明 |
|---|---|
| `math.lerp(a, b, t)` | 线性插值：`a + (b-a)*t` |
| `math.inv_lerp(a, b, v)` | 反插值：把 `v` 映射到 `[0,1]` 区间 |
| `math.remap(v, a, b, c, d)` | 把 `v` 从 `[a,b]` 重映射到 `[c,d]` |
| `math.clamp(v, lo, hi)` | 限制到 `[lo, hi]` |
| `math.smoothstep(e0, e1, x)` / `math.smootherstep(...)` | 平滑阶跃（Hermite / Quintic） |
| `math.sign(v)` | 返回 -1 / 0 / 1 |
| `math.wrap(v, len)` | 循环取模（始终落在 `[0, len)`） |
| `math.pingpong(t, len)` | 0→len→0 往复 |
| `math.move_towards(cur, target, max_delta)` | 以最大步长逼近目标 |
| `math.damp(cur, target, lambda, dt)` | 指数衰减逼近（帧率无关） |
| `math.angle_delta(a, b)` / `math.angle_lerp(a, b, t)` | 最短路径角度差 / 角度插值（弧度） |
| `math.round(v)` / `math.snap(v, step)` | 四舍五入 / 按步长吸附 |
| `math.approximately(a, b, eps?)` | 近似相等（默认 1e-6） |
| `math.ease_linear(t)` / `ease_in_quad` / `ease_out_quad` / `ease_in_out_quad` / `ease_in_out_cubic` / `ease_in_out_sine` 等 21 个缓动曲线 | 缓动函数 |

## 2.3 超高精度数值（`big.*`）

GryceSRT 内置任意精度十进制模块：`big` 提供大整数与超高精度浮点，
默认保留 **32 位小数**，可随时指定更高精度。所有函数以**字符串**返回。

```js
// 精确小数运算（0.1 + 0.2 == 0.3，不存在 double 误差）
big.decimal_add("0.1", "0.2");            // "0.3"

// 高精度除法 / 开方
big.decimal_div("1", "3");                // "0.333..."（32 位）
big.decimal_sqrt("2", 60);                // √2 精确到 60 位小数

// 大整数（Fibonacci(100) 精确值）
let x = big.int("0"), y = big.int("1");
for (let i = 0; i < 100; i++) { const t = big.int_add(x, y); x = y; y = t; }
// x == "354224848179261915075"
```

完整 API 见 [脚本 API 参考](./SCRIPT_API_REFERENCE.md) 第 5 节：
`big.int*`（int/int_add/int_sub/int_mul/int_div/int_mod/int_pow/int_neg/int_abs/int_compare/int_sign）
与 `big.decimal*`（decimal/decimal_add/decimal_sub/decimal_mul/decimal_div/decimal_pow/
decimal_sqrt/decimal_compare/decimal_floor/decimal_ceil/decimal_round/decimal_neg/
decimal_abs/decimal_sign）。

除零会抛出运行时错误（如 `big.int_div: division by zero`），可在 JS 侧用 `try/catch` 捕获。

## 3. 暴露属性（props）

脚本导出的 `props` 对象会被同步到组件并序列化：

```js
export const props = {
    speed: 2.5,        // 数字 -> 浮点 prop
    label: "player"    // 字符串 -> 字符串 prop
};
```

- Inspector 修改后立即写回 JS 模块的 `props` 对象（下一帧生效）。
- 随场景保存（.gesc）。
- 热重载（保存脚本/`ReloadScripts`）会保留 Inspector 里改过的值。

## 4. 示例脚本

可参考 GryceSRT API、[ECS 脚本指南](./ECS_SCRIPT_GUIDE.md) 和 GryceGC-A 标准创建
自己的 JS 脚本项目（`examples/` 已清空）。

## 5. GryceGC 打包（GryceSPC）

> GryceGC-A 项目组织与打包标准的完整说明见 [GryceGC-A 标准](./GryceGC-A.md)。示例项目中 `scripts/` 目录下的 JS 脚本位于 `.gesc` 场景文件同目录，打包时统一归入 `scripts` 类别。

### 5.1 构建游戏模板

```bat
cmake -B build/Release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/Release --target GryceGame
```

`../templates/GameTemplates.cpp`（GryceGame.exe）是一个独立入口：
Core 初始化 → 物理挂载 → Platform 创建窗口 → Renderer → 播放循环（脚本随 `GCore_BeginFrame` 运行）。

### 5.2 打包

```bat
build/bin/Release/GryceGC.exe --project <your-project-dir> --name MyGame ^
    --build-dir build --config Release --out build/game --author "Your Name"
```

输出 `build/game/MyGame/`（无 `res/` 目录）：

```text
MyGame.exe            游戏入口（唯一固定运行时，GryceGame 模板）
GFryceCore[d].dll 等     运行时 DLL 的加载期闭包镜像（MinGW 构建平铺到根目录）*
runtime/              核心运行时 DLL（GryceCore / Renderer / Platform / Physics / glfw）
                      + MSVC/MinGW/GCC 运行时（vcruntime/msvcp140 或 libgcc/libstdc++ 等，兜底用）
assets/*.gpkg         每个资源一个加密 .gpkg（GPAK v4，随机 Base64 名，
                      data 区 ChaCha20 加密；场景、脚本、着色器、模型、纹理等）
project.data          唯一状态文件（JSON）：清单 + 运行时设置 + 打包元数据
                      （源文件记录 path+SHA-256+size、64 字节 SHA-512 密钥
                      key_sha512_hex、ChaCha20 解密密钥 enc_key_hex、作者/项目/时间）
```

> `runtime/` 是引擎 DLL/GLFW/CRT 的规范化存放地。由于进程加载优先级是「exe 同目录 >
> runtime/」，`GryceGC` 会按工具链补齐根目录闭包：**MSVC**（`/DELAYLOAD`）只需把启动期静态
> 导入的 CRT 额外平铺到根目录；**MinGW**（无 `/DELAYLOAD`）把 `runtime/` 的完整闭包镜像到
> 根目录，保证 exe 与各引擎 DLL 在进程启动期的静态导入即可解析。

发布包加密：`GryceGC --pak --assets ./assets --output game.pak`（release 加密）会将
`.js` 编译为 QuickJS 字节码并 AES-256-GCM 加密、`.uif` DSL 文本加密，运行时由
`ResourceLoader` 解密加载（见 [脚本 API 参考](./SCRIPT_API_REFERENCE.md) 第 7 节）。

根目录 `project.gproj` 中的 `key_sha512_hex` 密钥由打包的源文件记录派生（SHA-512，64 字节，
hex 编码 128 字符），可用于校验包内容是否被改动；`.gpkg` 解密密钥 `enc_key_hex` 亦存于
`project.gproj`。作者等信息通过 `--author` 传入（默认取 `%USERNAME%`）。

### 5.3 运行

```bat
cd build/game/MyGame
MyGame.exe
MyGame.exe --scene res:/scenes/script_test.gesc   # 覆盖主场景
```

项目根默认取 exe 所在目录（`res:/` 以它为根），Core 启动时自动挂载
`assets/` 下的 `.gpkg`，并进入主场景（`project.gproj` 的 `main_scene`）。
编辑器菜单「文件 → 打包运行（GryceGC）」会自动完成打包。

运行时加载策略：MSVC 优先使用系统安装的 VC++ 运行时（System32）；系统缺失时，引擎 DLL
回退到 `runtime/` 里打包的运行时（Debug CRT 由 `grycegc` 额外平铺到根目录）；MinGW 的
CRT（libgcc/libstdc++/libwinpthread）通过根目录闭包在启动期解析。
