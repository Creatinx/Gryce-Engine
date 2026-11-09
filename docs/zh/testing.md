# 测试

本文描述 CTest 门禁的构成、用例清单与 CI 配置。

## 1. 门禁地位

CI 的通过条件只有一条：`ctest` 里的用例全部通过。任一用例失败，`ctest` 返回非零，job 即判定失败（CI 里没有 `continue-on-error`，也不会跳过用例）。配置阶段使用 `--no-tests=error`：万一 CMake 改动导致一条用例都没注册，ctest 会以错误退出，而不是"零用例零失败"地假绿。

```bash
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## 2. 用例清单

当前共 41 条用例，分七个宿主。

### 2.1 数学与资源规则（`GryceMathTests.exe`，7 条）

被测源码（`engine/math/*.cpp`、`engine/assets/material_rules.cpp`、`engine/assets/mesh_lod.cpp`）直接编译进测试可执行文件，**不链接任何引擎 DLL**，与 GPU、驱动、窗口 / 无头环境完全解耦，CI 上毫秒级出结果。全部为纯计算结果的数值核对，确定性，同一份代码在任何机器上结果一致。

| 用例 | 核对内容 |
| --- | --- |
| `math_vector` | `Vector2/3/4` 的加减乘除、点积叉积、长度与归一化，以及右手坐标系约定（X×Y=Z） |
| `math_matrix` | `Matrix4` 的 translate / scale / rotate / multiply / transpose / inverse，以及 perspective、ortho、look_at、billboard 的投影结果 |
| `math_quaternion` | 轴角构造、共轭与逆、乘法、slerp、`rotate_vector`，以及 `to_matrix` / `from_rotation_matrix` / 欧拉角往返的一致性 |
| `math_camera` | 默认朝向与右手正交基、`look_at` 反解 yaw/pitch、WASD 前后左右移动、pitch 限幅、视图与投影矩阵 |
| `math_ray` | `Ray::at` 求值、slab 法 AABB 求交（命中 / 未命中 / 内部起点）与命中距离 |
| `math_material` | 模型文件材质到 PBR 参数的推导规则（`assets/material_rules.cpp`，见 [model-import.md](model-import.md)） |
| `math_lod` | 自动 LOD 链的结构性保证（`assets/mesh_lod.cpp`，见 [lod.md](lod.md)） |

### 2.2 工具链（`GryceGCTests.exe`，11 条）

| 用例 | 核对内容 |
| --- | --- |
| `grycegc_cli` | 命令行解析与命令分发 |
| `grycegc_crc32` | CRC32 实现 |
| `grycegc_pe` | PE trailer / VERSIONINFO 读写 |
| `grycegc_aes` | AES-256-GCM 加解密 |
| `grycegc_smoke` | 端到端冒烟（临时工程上跑通主要命令） |
| `grycegc_crypto` | 加解密原语与容器格式 |
| `grycegc_pak` | `.gpack` / `.pak` 资源包 create / list / extract / verify |
| `grycegc_meta` | `gryce.meta.json` / `project.data` 元数据读写 |
| `grycegc_package` | 发布目录打包与 `verify` 自洽性 |
| `grycegc_webpack` | 单文件 HTML 打包输出结构 |
| `grycegc_new` | 项目骨架生成：文件清单、`project.data` / 场景 JSON 合法性、立方体 OBJ 顶点法线、非空目录拒绝与 `--force` 行为 |

### 2.3 脚本系统（`GryceScriptTests.exe`，6 条）

被测源码（`engine/script/*` + `runtime/engine_context` + glog + `res:/` 路径解析）直编进测试可执行文件，不链接引擎 DLL。脚本源文件写在系统临时目录（`temp/gryce_script_tests/<suite>`），每个 suite 前后自行建立 / 清理。用例设计见 [scripting.md](scripting.md)。

| 用例 | 核对内容 |
| --- | --- |
| `script_value_convert` | C++ 与 JS 值互转：标量往返、NaN / Infinity / 越界边界、Vector / Quaternion / Matrix4（列主序）数组形式、nlohmann::json 嵌套往返、`engine.*` 绑定冒烟 |
| `script_lifecycle` | awake / start / update / destroy 钩子顺序与时机，props 注入 `this.props`，帧时钟（frame 从 1 起算），`_input` 事件派发先于 update 且不消费 |
| `script_error_isolation` | 语法错误装载失败、运行期抛错后停止执行、死循环被帧预算中断；三种情况下组件状态翻转正确、引擎不崩 |
| `script_hot_reload` | 文件变更触发重编译且 props 跨重载保留；文件延迟出现时按轮询周期装载成功；`reload_now` 强制重载与 `set_script_path` 换脚本（命令面内核） |
| `script_app_entry` | 入口脚本（`main.js`）的 `on_start` / `on_update` / `on_destroy` 派发、热重载换模块（mtime + 大小判据）、扩展 ABI 的 `add_function` 注册与标量往返（见 [app-entry.md](app-entry.md) / [extension.md](extension.md)） |
| `script_component` | `engine.component.*` 脚本绑定：基类查询（`ColorRect` 能被当 `Component2D` 查到）、精确查询、父子挂载与摘除、`engine.component.children` 与 `engine.component.parent`、局部变换叠加、类型链名字 |

### 2.4 资产侧车文件（`GryceAssetTests.exe`，2 条）

被测源码（`engine/assets/asset_meta.cpp`、`engine/assets/import_options.cpp`、glog）直编进测试可执行文件，不链接引擎 DLL；用例在系统临时目录里自建 / 自清工程，不写仓库。

| 用例 | 核对内容 |
| --- | --- |
| `asset_meta` | 侧车读写往返、缺失时生成稳定 guid（32 位十六进制、512 次不重复）、缺失字段取默认值、坏 JSON 按缺失处理、非法设置回退但保留 guid、源文件指纹过期判定、源文件不存在时不写盘 |
| `asset_import_options` | 缩放、Z-up / X-up 上轴转换（顶点 / 法线 / 切线一起转）、材质开关、粗糙度倍率钳制、非法缩放被忽略（见 [core-api.md](core-api.md) 第 7 节） |

### 2.5 编辑命令面（`GryceCoreEditTests.exe`，3 条）

这是唯一**链接 `GryceCore.dll`** 的单元套件：命令消费、撤销栈、场景序列化都在 core 内部，没有可剥离的纯计算内核。这些路径不碰 GPU / 窗口（`GCore_Init` 只建 World 与场景），CI 上仍不需要显卡。

观测手段是"场景存档 JSON 逐字节比较"而不是实体计数：`GEntity_GetCount` 只统计有父节点的实体，拿它当断言依据会得到假阳性。

| 用例 | 核对内容 |
| --- | --- |
| `core_edit_edit` | 10 个 payload 结构体不超 256 字节、非法参数被拒（返回 0）、创建后自动选中、改名、`SetTransform` 的缺省分量补齐语义（见 [core-api.md](core-api.md) 第 1 节） |
| `core_edit_undo` | 创建后撤销回到逐字节相同的上一状态、重做精确复原、`BeginEdit`/`EndEdit` 把 5 次拖拽合并成 1 步、栈深上限、`SetUndoLimit(0)` 关闭记录、换文档清空历史 |
| `core_edit_scene` | 存 → 新场景 → 读 → 再存的两次存档逐字节相同（同时覆盖"序列化丢字段"与"反序列化不还原"）、加载不存在的路径返回 -1 且场景仍可用 |

### 2.6 组件模型（`GryceComponentTests.exe`，7 条）

被测源码（`engine/scene/entity.cpp` + `engine/scene/scene_serializer.cpp` + `engine/scene/prefab.cpp` + `engine/components/**` + `engine/reflection/**` + `engine/api/component_api.cpp` + `engine/api/core_api.cpp` + `engine/api/edit_api.cpp`）直编进测试可执行文件，不链引擎 DLL。覆盖 ECS 的**两条正交轴**（类型继承、实例挂载），语义见 [architecture.md](architecture.md) 3.1/3.2 与 [core-api.md](core-api.md) 第 8 章。

| 用例 | 核对内容 |
| --- | --- |
| `component_hierarchy` | 反射表逐条校验 `GRYCE_REFLECT_CLASS` 的 `Base` 是真实 C++ 基类；有派生类的基类（`Node` / `Node2D` / `Component2D` / `BasicRect`）必须覆写 `collect_type_views()`；`is_derived_from` 的自反 / 传递 / 非对称 |
| `component_query` | 基类查询（`NodeUI` 能被当 `Node2D` / `Node` / `Component` 查到；`ColorRect` 能被当 `BasicRect` / `Component2D` 查到）与精确查询互不为子集；Node 的域、Entity/组件树可见性传播和 NodeUI 序列化；同类型多实例的视角索引互不覆盖；卸载按指针精确擦除；`component_type_chain_names` 的顺序 |
| `component_pool` | 视角池的分配 / 释放 / 复用；池满拒绝；`view_at` 索引与池遍历一致 |
| `component_lifecycle` | awake / start / update / destroy 钩子顺序；`is_effectively_enabled` 的父禁用传播；组件禁用不影响兄弟 |
| `component_tree` | `set_component_parent` 防环、换父、摘除；删父组件后子组件重挂祖父且不失效；`component_root()`；局部变换叠在父组件之上（`world_transform_2d`）；`clone()` 重建树 |
| `component_serialize` | `.gesc` v3 的组件 `parent` 槽位号与 `local_transform` 落盘 / 读回；旧档（v1/v2，无这两字段）照读为"全根 + 单位变换"；`Prefab::revert` 按槽位重建树 |
| `component_capi` | 13 个 `engine.component.*` C API 的参数校验与返回约定；两个 setter（`SetParent` / `SetLocalTransform`）走命令缓冲而非直写，附 payload 尺寸与撤销往返断言 |

`component_capi` 的撤销断言是**命令化**的回归护栏：这两个 setter 若改回直写，`GEdit_BeginEdit` 抓不到快照，断言立即失败。

### 2.7 GryceUI（`GryceUiSkeletonTests.exe`，5 条）

只链接 `GryceCore.dll`，不创建窗口或 GPU 资源。测试 `NodeUI` 组件树布局、输入状态机和无后端 DrawList。

| 用例 | 核对内容 |
| --- | --- |
| `ui_layout` | 根节点填充视口、Column 布局、padding/gap 推进、最小尺寸、归一化锚点与本地偏移 |
| `ui_input` | hover/enter/leave、pointer down/up/click、focusable 祖先获取焦点、事件沿 NodeUI 父链冒泡、键盘与字符事件只进入焦点链 |
| `ui_drawlist` | UIPanel 背景、UILabel 文本、clip push/pop 顺序、字体尺寸与 RGBA 打包 |
| `ui_runtime` | 根树生命周期、隐藏节点移除布局、裁剪和根矩形共同限制命中、`clear()` 清理状态 |
| `ui_widgets` | Button/CheckBox/Slider 指针与键盘交互、TextInput UTF-8 编辑/IME 预编辑/Tab 焦点、ScrollView 滚轮与内容偏移 |

## 3. 执行注意

`grycegc_*` 用例并行执行会互相干扰（共享临时目录与进程级状态），本地全量验证请串行跑：

```bash
ctest --test-dir build -j1 --output-on-failure
```

CI 上各用例独立注册、由 runner 调度，不受此限。

## 4. 新增用例

- 数学 / 资源规则套件：在根 `CMakeLists.txt` 的 `GRYCE_MATH_TEST_SUITES` 里加名字，自动注册成一条 CTest。
- 资产 / 编辑命令面套件：分别在 `GRYCE_ASSET_TEST_SUITES`、`GRYCE_CORE_EDIT_TEST_SUITES` 里加名字。新增编辑命令面用例时注意两种返回约定（入队 1/0、查询与场景操作 0/-1），写错会让断言永远为真的假绿。
- GryceUI 骨架套件：在根 `CMakeLists.txt` 的 `GRYCE_UI_SKELETON_TEST_SUITES` 里加名字，运行 `GryceUiSkeletonTests.exe`。
- 组件套件：在根 `CMakeLists.txt` 的 `GRYCE_COMPONENT_TEST_SUITES` 里加名字。
- 工具链套件：在 `toolchain/CMakeLists.txt` 的 `_grycegc_suites` 里加名字。

## 5. 历史：像素比对用例已删除

早期的 `renderer_smoke_opengl` / `renderer_smoke_vulkan` 两条像素用例及其 Python 工具已删除：它们靠"截图里非背景像素占比"判定，受无头环境 / 软件渲染 / 驱动版本影响很大，既不反映构建正确性也不反映计算正确性。`GryceRenderSmoke.exe` 本身保留为人工运行的演示宿主；Web 端同理不进 CTest，只做人工验证（见 [webgl-backend.md](webgl-backend.md) 第 7 节）。

## 6. CI 配置

`.github/workflows/ci.yml`：任意 push / PR 触发，Windows / MSYS2 UCRT64，Release 配置，安装 GCC / CMake / Ninja / GLFW / GLEW / Box2D / Jolt 系统包（不装 Assimp 与 Vulkan SDK，对应路径自动降级），然后 configure + build + CTest。
