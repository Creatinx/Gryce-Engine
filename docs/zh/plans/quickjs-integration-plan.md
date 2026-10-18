# QuickJS 脚本引擎接入计划

> 状态：**已实施**（2026-10-18 制定并当日完成 M0–M5；落地口径与偏差见文末第 8 节）
> 目标：把仓库内已 vendor 的 QuickJS（2026-06-04，MIT）接入引擎，提供 ECS `ScriptComponent` + 每帧 `update(dt)` 的 JavaScript 脚本能力：沙箱化、可热重载、四后端（GL/Vulkan/D3D12/WebGL）一致可用。
> 估算：**引擎侧新增约 1,950–2,400 行**（运行时封装 + 值转换 + 绑定 + 组件 + 测试 + 文档；明细见 3.7），单人 **8–12 个工作日**（碎片时间约 2 周）。
> 对比基准：QuickJS 本体约 1.4 万行 C 已随仓库分发（`third_party/quickjs/`），**原样保留、零修改**，本计划只动它的 CMake 源列表；引擎侧新增量约为 WebGL 后端接入（约 1,200–2,100 行）同量级。

---

## 0. 范围

**做**：

1. 构建接线：根 CMakeLists 增加 `add_subdirectory(third_party/quickjs)`，`GryceQuickJS` 静态库链入 `GryceCore`；Emscripten（静态库单 wasm）与桌面（DLL）两条路径都验证。
2. 新建 `engine/script/`：JS 运行时封装（`JSRuntime`/`JSContext` 生命周期、内存上限、中断处理）、C++↔JS 值转换层、`engine.*` 原生绑定（log / time / 实体与 Transform 读写 / 输入增量 / 物理重力）。
3. `ScriptComponent`（挂 Entity：脚本路径 + props 表 + 运行时实例）与生命周期调度（`awake` / `start` / `update(dt)` / `destroy`），序列化走既有通用通道。
4. 沙箱：只编译 QuickJS 核心 5 个 C 文件，不启用 `std`/`os` 模块；`console.log` 自绑定到 glog；运行时内存上限 + 每帧 opcode 预算。
5. 资源加载：`res:/scripts/*.js` 解析（复用现有 `res:` 前缀与打包归档体系）。
6. 热重载：脚本文件 mtime 轮询 + `snapshot/restore_runtime_state` 保留 props 后重建实例。
7. CTest 新增 `script` 套件（值转换 / 生命周期 / 错误隔离 / 热重载），CI 门禁同步。
8. 文档：`docs/zh/scripting.md` + `docs/en/scripting.md`，两套 README 索引同步。

**不做**（明确排除）：

- 编辑器侧 UI（脚本检查器面板、`ECMD_SET_SCRIPT`/`ECMD_RELOAD_SCRIPTS` 命令的发送方）。命令枚举已在 `engine/api/types.h:99-101` 预留，本计划只保证命令语义在引擎侧可被消费的最小闭环，编辑器不在本仓库。
- TypeScript/源码映射/打包压缩链；v1 直接执行 `.js` 源码。
- async/await 事件循环集成：v1 每帧末执行 pending job（Promise 微任务），不引入独立 loop。
- 每脚本独立 `JSRuntime` / worker 隔离（内存与原子表开销过大，且四后端一致更难保证）。
- 实现 `engine/api/script_api.h` 里已废弃的 `GScript_RunString/RunFile`（保留声明与 deprecated 标注，作为外部宿主 C ABI 桥的历史痕迹）。
- 移动端 / 原生平台移植（本计划只覆盖桌面三后端 + WebGL，与现有 CI/构建矩阵一致）。

---

## 1. 已核实的前置条件

以下均在本仓库代码里实测确认（不是推测）：

| 前提 | 状态 | 证据 |
|---|---|---|
| QuickJS 源码已 vendor 且带独立 CMake（`GryceQuickJS` 静态库、MinGW 定义、C11、PIC、告警屏蔽全部预置） | 是 | `third_party/quickjs/CMakeLists.txt`（`add_library(GryceQuickJS STATIC ...)`、`CONFIG_VERSION="2026-06-04"`、`_GNU_SOURCE`/`__USE_MINGW_ANSI_STDIO`、`POSITION_INDEPENDENT_CODE ON`） |
| **该 CMake 从未被构建过**（`add_subdirectory` 无任何调用点） | 是（需 M0 验证） | 根 `CMakeLists.txt:371` 仅 `add_subdirectory(engine)`、`:576` 为 toolchain；`engine/CMakeLists.txt:13` 只在注释里提到 GryceQuickJS |
| QuickJS 版本与许可 | 是 | `third_party/quickjs/Changelog` 首行 `2026-06-04`；`third_party/quickjs/LICENSE`（MIT，与 AGPL 主体兼容；third_party 各库保留原许可是仓库既定口径） |
| 组件基类已具备脚本所需的全部生命周期与热重载快照钩子 | 是 | `engine/components/component.h:31-44`（`on_awake/on_start/on_update(dt)/on_destroy`、`snapshot_runtime_state/restore_runtime_state` 注释即"用于热重载保留"） |
| 场景每帧已逐组件驱动 `on_update(dt)`——脚本组件零场景改动即可运行 | 是 | `engine/scene/entity.cpp:125-131`、`engine/scene/scene.cpp:118` |
| 序列化/反序列化按 `comp->type()` 字符串通用分发（新组件零序列化改动） | 是 | `engine/scene/entity.cpp:314-328,353-356`（`ComponentFactory::create(comp->type())`）、`engine/scene/prefab.cpp:344,457-473` |
| 组件类型注册表 + 集中注册函数 + 反射集中注册风格 | 是 | `engine/components/component_factory.h:24-59`（`register_type`/`ComponentRegistrar`/`register_builtin_components`）、`engine/reflection/builtin_reflections.cpp:1-40`（"仅保留渲染相关组件反射（……脚本子系统已裁剪）"，按同风格补注册） |
| 引擎内部已按"脚本经 `engine.*` 命名空间调用"埋点 | 是 | `engine/ecs/system.h:54`（"脚本经 engine.physics 调用"）、`engine/api/core_api.cpp:648`（鼠标增量"供脚本在 on_update 读取本帧增量"）、`engine/api/core_api.cpp:575`（"scenes, shaders, scripts, assets resolve from packaged archives"） |
| 编辑器命令总线已预留脚本命令位 | 是 | `engine/api/types.h:99-101`（`ECMD_SET_SCRIPT = 500`、`ECMD_RELOAD_SCRIPTS`） |
| 遗留 C ABI（GryceSRT）含 props 查询/设置（float/string），且自述废弃、指向 "ECS ScriptComponent with QuickJS" | 是 | `engine/api/script_api.h`（`GScript_GetPropCount/GetPropInfo/GetPropFloat/SetPropFloat/GetPropString/SetPropString`；无 `.cpp` 实现） |
| CTest 模式成熟：源码直编被测单元、`--suite` 一条一例、不链引擎 DLL | 是 | `test/math_tests.cpp:1-30`、根 `CMakeLists.txt:521-563` |
| glog 自包含、无引擎依赖，可单独进测试 exe | 是 | `engine/utils/glog/glog_lib.cpp`（math 测试即如此链接）；`GLOG_*` 宏见 `engine/utils/glog/glog.h:273-278` |
| `res:` 资源前缀与打包归档解析已存在 | 是 | `test/demo_project/project.data`（`"entry_scene": "res:/scenes/main.gesc"`）、`engine/resources/pak_bundle.cpp` 等 |
| Emscripten 下引擎三库均为 STATIC、单 wasm，第三方静态库可直接链 | 是 | `engine/CMakeLists.txt:21-25`（`_gryce_lib_type STATIC`）、根 `CMakeLists.txt:385-520` |
| CI 不引入新联网依赖（QuickJS 随仓库分发） | 是 | `cmake/GryceFetchDeps.cmake` 唯一联网依赖是 assimp；quickjs 为 vendor 源码，configure 零下载 |

---

## 2. 关键设计决策

### 2.1 一个 `JSRuntime` 全局共享，每个 `ScriptComponent` 一个 `JSContext`

`JSRuntime` 持有堆、原子表与 GC 状态，按引擎进程一个（懒创建，随引擎 shutdown 释放）；`JSContext` 轻量，每组件实例一个，天然提供实例间全局对象隔离（Unity MonoBehaviour 式心智模型）。不采用每组件独立 runtime：原子表与堆按 runtime 分配，数百实体下内存与启动开销不可接受。GC 由运行时封装在低负载时机调用 `JS_RunGC`（借鉴 QuickJS 官方的 idle GC 思路，不引入后台线程——Emscripten 无 pthread 的三处退化已有先例，`docs/zh/webgl-backend.md`）。

### 2.2 直接绑定内部 C++ API，不绕 `script_api.h` 的 C ABI

脚本模块编译进 `GryceCore`（桌面为 DLL 内部调用，无 ABI 外泄问题），直接绑定 `scene::Entity`/`components::Transform` 等内部类型：调用快、类型全、可传引用。`engine/api/script_api.h` 的 `GScript_*` C ABI 面向编辑器/外部宿主（`GEntityHandle` 为 int、props 只有 float/string、错误走 char buffer），作为桥接过重；保留其声明与 deprecated 标注，不实现、不删除。

### 2.3 沙箱：只编 QuickJS 核心 5 个 C 文件，不编 `quickjs-libc.c`

`quickjs-libc.c` 提供 `std`/`os` 两个模块（文件系统、进程、时间），对游戏脚本是攻击面也是不确定性来源。v1 的 `GryceQuickJS` 源列表收窄为：`quickjs.c` / `cutils.c` / `dtoa.c` / `libregexp.c` / `libunicode.c`，**不调用** `js_init_module_std/os`，`console.log/warn/error` 由引擎原生函数绑定到 `GLOG_*`。收益：沙箱默认无文件访问；`quickjs-libc.c` 单文件约 13 万字符也不再进入编译。代价：脚本无 `require`——v1 用不到（每组件单文件）。

### 2.4 `EntityID`（uint64）不以原始值进 JS

`ecs::EntityID` 是 `uint64`（`engine/ecs/types.h:12`），超过 2^53 后 JS number 丢精度。方案：`ScriptComponent` 所在 Scene 维护一张 `EntityID ↔ int32` 局部 handle 表（与 C ABI 的 `GEntityHandle` 为 int 的口径一致），脚本侧实体句柄用 int32；反查走同一张表。表在 Scene 加载/实体销毁时同步（复用 `entity_handle_map` 的思路，`engine/runtime/entity_handle_map.cpp`）。

### 2.5 组件自驱动 `on_update`，不新增 `ScriptSystem`

`Entity::on_update` 已逐组件调用 `on_update(dt)`（`entity.cpp:125-131`），`ScriptComponent::on_update` 内直接执行 JS `update(dt)` 即可，零场景/零系统改动。热重载的文件轮询放组件内（mtime 缓存，约 0.5s 一次 `stat`，数百实体成本可忽略）。若未来出现跨实体批处理、脚本间消息总线需求，再按 `AnimatorSystem` 模式新增 `ScriptSystem`（`engine/ecs/systems/animator_system.h` 为范本），届时组件内驱动可降级为系统代发。

### 2.6 值转换集中一处，显式特化

`engine/script/js_value.h` 对 POD（float/int/bool/std::string）与引擎数学类型（`Vector2f/Vector3f/Matrix4f` 等，`engine/math/math.h`）做显式 `to_js`/`from_js` 特化；各绑定点只调转换函数，不允许在绑定代码里手写 `JS_ToFloat64` 等。整数/浮点边界（NaN、Infinity、越界截断）在转换层一次定义、测试覆盖。

### 2.7 props 用 nlohmann::json，与组件序列化同构

`ScriptComponent` 序列化字段：`script`（`res:/scripts/xxx.js`）+ `props`（json 对象）。场景加载后 `on_start` 前把 props 注入为脚本内的 `this.props`（组件实例对象）；编辑器/外部经 props 表读写（对应既有 `GScript_GetProp*` 的 float/string 口径，后续可扩类型）。props 与运行时实例分离：热重载只重建 JS 侧，props 不丢（`snapshot_runtime_state` 的语义即为此设计）。

### 2.8 每帧末驱动 pending job，不引入事件循环

每帧所有脚本 `update` 执行完后，运行时封装统一 `JS_ExecutePendingJob(runtime)`（有界次数，如 1024，防 Promise 链自激）。v1 不提供 setTimeout/DOM 式异步。

---

## 3. 代码改动清单

### 3.1 第三方 / 构建（小改，两处）

- `third_party/quickjs/CMakeLists.txt`：源列表移除 `quickjs-libc.c`（见 2.3）；补一条注释说明沙箱口径。其余（MinGW 定义、PIC、C11、告警屏蔽）原样保留。
- 根 `CMakeLists.txt`：在 `add_subdirectory(engine)`（`:371`）前增加 `add_subdirectory(third_party/quickjs)`；`engine/CMakeLists.txt` 中 `target_link_libraries(GryceCore PRIVATE GryceQuickJS)`，并把 `:13` 注释里的"仅注释提及"改为实际链接说明。

注意点：`GryceQuickJS` 是 C 目标、`GryceCore` 是 C++ 目标，链接用 PRIVATE（脚本 API 不出 DLL）；Emscripten 分支无需改动（STATIC + PIC 已适配）。

### 3.2 新目录 `engine/script/`（约 1,160 行）

| 文件 | 行数 | 职责 |
|---|---|---|
| `script/js_runtime.h/.cpp` | ~200 | `JSRuntime`/`JSContext` 生命周期（懒创建、引用计数或引擎 shutdown 释放）、`JS_SetMemoryLimit`、`JS_SetInterruptHandler`（帧预算）、`eval_module`（ES module 求值 + 异常捕获）、`run_gc` |
| `script/js_value.h` | ~280 | 值转换层（2.6）：POD + `math::` 类型 + nlohmann::json 互转 |
| `script/js_bindings.h/.cpp` | ~380 | `engine.*` 命名空间：`engine.log`（→GLOG）、`engine.time`（帧计数/dt/总时长）、`engine.entity`（句柄表查询、Transform/position/rotation 读写）、`engine.input`（本帧键鼠增量，接 `core_api.cpp:648` 同源数据）、`engine.physics.gravity`（接 `system.h:54` 预留的重力接口） |
| `script/script_component.h/.cpp` | ~300 | `ScriptComponent`：`type()="Script"`、serialize/deserialize（script 路径 + props）、`on_attach/on_awake/on_start/on_update/on_destroy`、mtime 轮询热重载、错误隔离状态机（failed 标记 + 退避）、EntityID↔int32 句柄表 |

### 3.3 组件注册（约 20 行，零序列化改动）

- `engine/components/component_factory.cpp` 的 `register_builtin_components()`：注册 `Script`（category `Other`，description 说明用途）。
- `engine/reflection/builtin_reflections.cpp`：按集中注册风格补 `ScriptComponent` 的 props 字段反射（`script` 字符串 + props json），供编辑器属性面板。
- 场景序列化器 / prefab：**零改动**（1. 已核实：按 `type()` 通用分发）。

### 3.4 资源加载（约 40 行，内嵌于 3.2）

脚本源码读取：`res:/scripts/xxx.js` 经现有资源路径解析（pak 包内优先、磁盘回退），同步读入 `std::string`。v1 不做异步加载（脚本体积 KB 级；`async_loader` 留给大资源）。

### 3.5 CTest（约 420 行）

新增 `test/script_tests.cpp` + 根 CMakeLists 仿 `GRYCE_MATH_TEST_SUITES`（`:541-562`）注册 `GryceScriptTests`，四个 suite：

| suite | 覆盖 |
|---|---|
| `value_convert` | 2.6 转换层：float/int/bool/string/Vector3/Matrix4/json 往返，NaN/Infinity/越界边界 |
| `lifecycle` | awake→start→update→destroy 顺序；props 注入时序；禁用组件不执行 |
| `error_isolation` | 语法错误 / 运行期抛错 / 死循环中断（中断处理器生效）后引擎不崩、后续帧按退避策略跳过 |
| `hot_reload` | 改文件 → mtime 检出 → 重建实例 → props 保留（临时目录写文件，CI 可写） |

被测源码直编进 exe（`script/` + `scene/entity.cpp` 等最小集 + glog），不链 `GryceCore.dll`——与 math 测试同构，CI 毫秒级、与 GPU/无头环境无关。

### 3.6 文档（约 350 行，zh/en 各半）

- `docs/zh/scripting.md` / `docs/en/scripting.md`：脚本模型（组件/生命周期/props）、`engine.*` API 参考、沙箱与限制、热重载、排错表（对应计划 M5 交付）。
- `docs/zh/README.md`、`docs/README.md`、`docs/en/README.md`：模块表与 plans 归档说明各加一行（本计划文档同步入索引）。

### 3.7 代码量估算（分行数，只算新增/改动）

| 块 | 行数 |
|---|---|
| `engine/script/`（3.2 + 3.4） | ~1,200 |
| 组件注册 + 反射（3.3） | ~20 |
| CMake 两处（3.1） | ~25 |
| `test/script_tests.cpp` + CMake 注册（3.5） | ~420 |
| 文档 zh/en（3.6） | ~350 |
| **合计** | **~2,000**（区间 1,950–2,400，含异常分支与注释） |

---

## 4. 里程碑（每阶段可验证产物）

- **M0 构建接线**：`add_subdirectory` + 链接 + 一个 `eval("1+1")` 冒烟（可临时放 `GryceRenderSmoke` 启动路径或脚本测试雏形）。验证三套构建目录全过：`build/`（Debug + Vulkan + D3D12）、`build-ci2`（Release + `-DCMAKE_DISABLE_FIND_PACKAGE_Vulkan=ON`，忠实 CI）、`build-webgl`（Emscripten）。**这是本计划最大不确定点**（该 CMake 从未构建过，见 1.）。
- **M1 运行时 + 转换层**：`js_runtime`/`js_value` 落地，`value_convert` suite 绿。
- **M2 ScriptComponent 生命周期**：注册 + 序列化 + props 注入 + `update(dt)` 驱动；demo 场景（`test/demo_project`）加一个示例脚本（如绕 Y 轴匀速旋转 + `engine.log` 输出）验证 `res:/scripts/` 加载；`lifecycle` suite 绿。
- **M3 沙箱与错误隔离**：内存上限、中断处理、`console`→GLOG 绑定；`error_isolation` suite 绿；验证无 `std`/`os` 时 `require('fs')` 抛错而非静默。
- **M4 热重载 + 命令闭环**：mtime 轮询 + 快照恢复；`hot_reload` suite 绿；`ECMD_SET_SCRIPT`/`ECMD_RELOAD_SCRIPTS` 的引擎侧消费最小实现（命令语义可被现有命令总线派发到组件）。
- **M5 文档 + 全量收口**：scripting.md zh/en + 三处索引；四后端（GL/Vulkan/D3D12/WebGL 单文件包）人工运行验证；CTest 17+N 全绿；CI 绿。

---

## 5. 平台差异清单（逐条实现时验证）

| 平台 | 关注点 | 处置 |
|---|---|---|
| Windows / MSYS2 UCRT64（主路径） | `_GNU_SOURCE`/`__USE_MINGW_ANSI_STDIO` 已预置；dtoa 的 locale 行为 | M0 编译 + `value_convert` 数字往返验证 |
| Windows / MSVC（CLion 用户可能选用） | `quickjs.c` 单文件 2MB，MSVC 编译慢/可能需 `/bigobj` | 主路径仍是 UCRT64；MSVC 仅记录，不承诺 |
| Emscripten / WebGL | 纯 C11 可移植；无 pthread 需求；STATIC 链入单 wasm | M0 在 `build-webgl` 验证；禁 os 模块后无 fs 副作用 |
| CI（无 Vulkan 包） | quickjs 无条件编译，与 `GRYCE_HAS_VULKAN` 正交，不影响既有 CI 复现口径（`-DCMAKE_DISABLE_FIND_PACKAGE_Vulkan=ON`） | M0 在 `build-ci2` 复验 |
| 64 位实体 ID | uint64→JS number 精度（2.3 已述） | int32 局部 handle 表 + `value_convert` 边界用例 |

---

## 6. 验收标准

1. `ctest -R script` 四个 suite 全绿；math/grycegc 既有 17 条无回归（总量 21 条）。
2. CI（GitHub Actions，无 Vulkan 环境）全绿——M0 起每次推送即验证。
3. demo 场景示例脚本在 GL / Vulkan / D3D12 / WebGL（`GryceGC webpack` 单文件包）四端行为一致（同一 `update(dt)` 语义、日志可见）。
4. 无脚本场景零回归：不挂 `ScriptComponent` 的实体/场景每帧路径与接入前逐字节一致（组件不存在即无代码路径）。
5. 沙箱验证：脚本内 `require`/文件访问/进程访问均抛错；死循环脚本在帧预算内被中断且引擎继续运行。
6. 热重载验证：运行中改 `.js` 文件，行为更新且 props 不丢、无误报销毁。

---

## 7. 风险登记

| 风险 | 影响 | 缓解 |
|---|---|---|
| **`GryceQuickJS` 从未构建过**（add_subdirectory 从未调用），QuickJS 2026-06-04 与本工具链存在未遇过的编译/告警问题 | M0 卡住 | M0 第一件事单独构建该目标；上游 CMake 已预置 MinGW 定义；最坏情况收窄编译选项（`-w` 已有）或修第三方补丁（记录到 third_party 许可口径内） |
| `quickjs.c` 2MB 单文件编译内存/时长（CI runner） | CI 时长上升 | UCRT64 gcc 实测；必要时拆 `-j` 并行已有（CI `--parallel`）；监控单文件编译时长 |
| 热重载与场景保存/加载竞态（编辑器不在本仓，但 demo/自动化可能同时写文件） | 实例状态错乱 | 轮询只在运行期进行；props 单一数据源在组件；`restore_runtime_state` 失败即整体重建并告警 |
| 脚本抛错路径刷屏日志 | 日志洪水 | 组件内 failed 状态 + 退避（如每 N 帧或错误内容变化才再报） |
| `engine.*` 绑定面随需求膨胀 | 计划蔓延 | v1 绑定面按 3.2 表冻结；新增绑定走后续增量计划 |
| Emscripten 下 QuickJS 的 dtoa/unicode 表体积 | wasm 包变大 | `libunicode.c` 保留（正则/Unicode 必需）；量级 MB 以内可接受，M0 记录包体增量 |
| WebGL 单文件包（`GryceGC webpack`）未包含新脚本资源 | 演示断链 | M5 验证打包流；脚本走 `res:` 与场景同机制，预期零改动 |

---

## 8. 一句话结论

QuickJS 本体已在仓库内且许可兼容，缺的只是"最后一次接线"：把从未被构建的 `GryceQuickJS` 链进 `GryceCore`，用约 2,000 行引擎侧代码（运行时封装 + 值转换 + `engine.*` 绑定 + `ScriptComponent` + 测试与文档）换来沙箱化、可热重载、四后端一致的 JavaScript 脚本能力；组件基类、序列化通用分发、CTest 模式、`res:` 资源体系全部现成，真正的风险只有一个——M0 首次构建那个从未编译过的第三方目标。

---

## 9. 实施结果与偏差（2026-10-18）

实现落地后的实际口径，与上文有出入处以本节为准。

| 项 | 计划 | 实施结果 |
| --- | --- | --- |
| 模块求值 | 直接 `JS_Eval(JS_EVAL_TYPE_MODULE)` 取模块记录 | **该路径返回执行后的 Promise**（`js_evaluate_module` → `m->promise`）。改为 `\| JS_EVAL_FLAG_COMPILE_ONLY` → 保留模块引用 → `JS_EvalFunction` 执行 → **之后** `JS_GetModuleNamespace`；顺序颠倒（先取命名空间）会解引用未求值的导出绑定而崩溃 |
| 热重载判据 | mtime | **mtime + 文件大小**双判据（FAT / 网络盘 / 部分虚拟盘写入时间粒度 1~2 秒，同 tick 连续保存只比 mtime 会漏检） |
| 错误日志 | 带 stack | `name: message` + 换行 + stack；只取 `stack` 会丢掉异常主体 |
| 日志 API | `console.log` 绑 glog | 已实现（多参数按空格拼接），与 `engine.log.*` 同源；后者保留作为引擎风格入口 |
| 命令面 | `ECMD_SET_SCRIPT` / `ECMD_RELOAD_SCRIPTS` 最小消费 | 已实现于 `api/core_api.cpp`：换脚本立即装载（props 保留，空串卸载）、全场景强制重载 |
| 测试量 | 4 suite / 21 条门禁 | 4 suite **17 例 97 断言**，CTest 仍为 21 条（每个 suite 一条） |
| 关于 `awake` | 组件挂载后调用 | 编译成功才调用；装载失败时脚本侧无从执行（错误已记录） |
| 平台 | 四后端一致可用 | GL / Vulkan / D3D12 跑 `main.gesc`、WebGL（wasm）跑 `webgl_demo`，均见脚本 `start` 日志；`webgl_demo` 立方体旋转经截图差异确认生效 |

验证入口（`--simulate` 才进 Play 模式驱动 `on_update`）：

```bash
GryceRenderSmoke.exe --api opengl --project test/demo_project \
    --scene test/demo_project/scenes/main.gesc --frames 60 --simulate \
    --shot shot.png --no-debug-ui --no-fps
```
