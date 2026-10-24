# 测试

本文描述 CTest 门禁的构成、用例清单与 CI 配置。

## 1. 门禁地位

CI 的通过条件只有一条：`ctest` 里的用例全部通过。任一用例失败，`ctest` 返回非零，job 即判定失败（CI 里没有 `continue-on-error`，也不会跳过用例）。配置阶段使用 `--no-tests=error`：万一 CMake 改动导致一条用例都没注册，ctest 会以错误退出，而不是"零用例零失败"地假绿。

```bash
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## 2. 用例清单

当前共 40 条用例，分六个宿主。

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

### 2.3 脚本系统（`GryceScriptTests.exe`，5 条）

被测源码（`engine/script/*` + `runtime/engine_context` + glog + `res:/` 路径解析）直编进测试可执行文件，不链接引擎 DLL。脚本源文件写在系统临时目录（`temp/gryce_script_tests/<suite>`），每个 suite 前后自行建立 / 清理。用例设计见 [scripting.md](scripting.md)。

| 用例 | 核对内容 |
| --- | --- |
| `script_value_convert` | C++ 与 JS 值互转：标量往返、NaN / Infinity / 越界边界、Vector / Quaternion / Matrix4（列主序）数组形式、nlohmann::json 嵌套往返、`engine.*` 绑定冒烟 |
| `script_lifecycle` | awake / start / update / destroy 钩子顺序与时机，props 注入 `this.props`，帧时钟（frame 从 1 起算），`_input` 事件派发先于 update 且不消费 |
| `script_error_isolation` | 语法错误装载失败、运行期抛错后停止执行、死循环被帧预算中断；三种情况下组件状态翻转正确、引擎不崩 |
| `script_hot_reload` | 文件变更触发重编译且 props 跨重载保留；文件延迟出现时按轮询周期装载成功；`reload_now` 强制重载与 `set_script_path` 换脚本（命令面内核） |
| `script_app_entry` | 入口脚本（`main.js`）的 `on_start` / `on_update` / `on_destroy` 派发、热重载换模块（mtime + 大小判据）、扩展 ABI 的 `add_function` 注册与标量往返（见 [app-entry.md](app-entry.md) / [extension.md](extension.md)） |

### 2.4 资产侧车文件（`GryceAssetTests.exe`，2 条）

被测源码（`engine/assets/asset_meta.cpp`、`engine/assets/import_options.cpp`、glog）直编进测试可执行文件，不链接引擎 DLL；用例在系统临时目录里自建 / 自清工程，不写仓库。

| 用例 | 核对内容 |
| --- | --- |
| `asset_meta` | 侧车读写往返、缺失时生成稳定 guid（32 位十六进制、512 次不重复）、缺失字段取默认值、坏 JSON 按缺失处理、非法设置回退但保留 guid、源文件指纹过期判定、源文件不存在时不写盘 |
| `asset_import_options` | 缩放、Z-up / X-up 上轴转换（顶点 / 法线 / 切线一起转）、材质开关、粗糙度倍率钳制、非法缩放被忽略（见 [editor-api.md](editor-api.md) 第 7 节） |

### 2.5 编辑器接口（`GryceEditorTests.exe`，3 条）

这是唯一**链接 `GryceCore.dll`** 的单元套件：命令消费、撤销栈、场景序列化都在 core 内部，没有可剥离的纯计算内核。这些路径不碰 GPU / 窗口（`GCore_Init` 只建 World 与场景），CI 上仍不需要显卡。

观测手段是"场景存档 JSON 逐字节比较"而不是实体计数：`GEntity_GetCount` 只统计有父节点的实体，拿它当断言依据会得到假阳性。

| 用例 | 核对内容 |
| --- | --- |
| `editor_edit` | 10 个 payload 结构体不超 256 字节、非法参数被拒（返回 0）、创建后自动选中、改名、`SetTransform` 的缺省分量补齐语义（见 [editor-api.md](editor-api.md) 第 1 节） |
| `editor_undo` | 创建后撤销回到逐字节相同的上一状态、重做精确复原、`BeginEdit`/`EndEdit` 把 5 次拖拽合并成 1 步、栈深上限、`SetUndoLimit(0)` 关闭记录、换文档清空历史 |
| `editor_scene` | 存 → 新场景 → 读 → 再存的两次存档逐字节相同（同时覆盖"序列化丢字段"与"反序列化不还原"）、加载不存在的路径返回 -1 且场景仍可用 |

### 2.6 UI 模型层与操作队列（`GryceUiTests.exe`，15 条）

被测源码（`engine/api/ui_op_queue.cpp` + `engine/api/ui_api.cpp` + `engine/ui/*.cpp`）直编进测试可执行文件，不链 GryceRenderer.dll；且**不定义 `GRYCE_HAS_IMGUI`**——同时验证"关掉 ImGui 的构建里队列与模型层照样能用"（无窗口宿主也走这条路）。`GRYCE_RENDERER_BUILDING` 让导出宏变成 dllexport，静态 exe 内自链接。

`queue` 套件测 OpQueue 本体：FIFO 顺序与 seq、同键就地合并（位置与 seq 不动）、同 key 不同 type 不合并、离散项绝不合并、满队只逐最旧可合并项、满队全是离散项时拒绝入队并计数、排出预算、排出间隔（首次不受限、空排不消耗间隔）、`clear` 不清累计统计、排出后同键是新的一条、坏参数不动队列。

`abi` 套件测 C 接口层：字段往返与 seq 由引擎填写、经 ABI 的合并与离散、策略读写、间隔节流、`GUi_ClearOps`、统计与查询容忍 nullptr、坏参数拒绝、请求超过容量时按容量封顶。ABI 作用在进程内唯一队列上，用例之间靠"清空 + 推进若干帧"隔离残留的间隔状态。

`state` / `layout` / `theme` / `input` / `snapshot` 五个套件测 `engine/ui/` 模型层：稳定 id 与状态存储（老化边界、截断、清空、计数 ABI）、确定性布局栈（嵌套面板、行高取 `max(行高, 最高格)`、自动收行、缩进）、主题 token（5 套预设锚色、预设数量与名字、派生 token 跨预设一致性、逐项覆盖不跨预设存活、越界容忍、无 ImGui 时 apply 为 no-op、圆角/滚动条度量）、输入门控与快捷键表（精确匹配修饰键、注册期报冲突、容量）、面板视图模型的 JSON 往返（64 位 id 走十进制字符串、坏行跳过、宽容读取、容量截断）。

`comp` 套件测组件树布局：add/find/remove 与子树连带删除、父级必须存在、容量上限、子节点顺序、`set` 换父级做环检查、**非布局父级保留直写 pos**、Column 堆叠、Row 交叉轴四种对齐、flex 分配主轴剩余空间、溢出钳到 0、Grid 等宽列与行高、padding 缩内容区、嵌套树、退化容器。`compabi` 套件测同一套语义经 C 面的往返（spec 字段、越界枚举拒绝、坏参数返回约定）。

`anim` 套件测动画系统：块一纯函数（11 种缓动的边界对齐与区间夹取、回弹过冲后回落、补间的延迟/越界/零时长、颜色插值夹取、关键帧有序插入与同刻覆盖、区间外保持、段缓动取自前一个关键帧、容量）与块二运行时（片段时长推导与同名轨道覆盖、Once 播完置 finished 且停在终点、Loop 回绕与 PingPong 折叠、速度/seek/暂停/停止、轨道 delay 期间保持初始值、颜色通道与类型纪律、换片段重建通道表、无片段时全操作 no-op）。`animabi` 套件测 C 面：缓动与名字的模型一致性（含越界枚举）、补间/颜色助手、以及"建 C 片段 → 播放 → 推进 → 读通道 → 暂停/变速/seek/停止"的完整闭环。

队列语义与策略的完整说明见 [ui-api.md](ui-api.md) 第 7.5 节，组件树布局见第 7.7 节，主题预设与美化见第 7.8 节，动画系统两块见第 7.9 节，内置 UI 渲染路径与 UIDrawList 见第 7.10 节，双轨切换与 `GUi_*` 能力表见第 7.11 节。

`drawlist` 套件测 UI 自绘命令缓冲（内置 UI 渲染路径的模型层，见 ui-api.md 第 7.10 节）：命令顺序与字段往返、空文本/nullptr 不产生命令、字符串池多条互不重叠、命令带裁剪深度标记、嵌套裁剪取交集、空裁剪剔除绘制命令且 clip 命令照常记录、pop 恢复父裁剪、空栈 pop 无操作、三层嵌套交集、裁剪栈满拒绝 push 且 pop 不失衡、命令表满丢弃并计数、字符串池放不下整条丢弃不截断、reset 清空裁剪栈、负几何原样记录。

`bridge` 套件测 drawlist → `UiRenderer` 的桥：命令按序翻译成绘制的矩形/文本/裁剪、裁剪深度只在变化时下发、桥自持裁剪栈以便自我描述、文本行盒顶换算成基线、颜色 `0xRRGGBBAA` 逐通道归一化、字符串池的 off 与 len 正确切片、空 drawlist 不产生绘制。桥不碰任何后端 API，所以断言落在"记录下来的绘制调用序列"上。

`widget` 套件测保留模式控件层（阶段 3）：命中测试与"同帧后提交者拿按下"的层级规则、按下/拖出取消/松开成点击、Checkbox 与 Slider 的拖拽与外部值同步、TextInput 的焦点转移与字符/退格消费、`ensure` 与 `touch` 的区别（前者不清标志、后者清）、`sweep` 回收本帧未提交的槽、无窗口时全部是空操作、窗口退化尺寸被拒。

`builtin` 套件测引擎内置的立即模式层（阶段 4a/4b，不依赖 ImGui）：条目自上而下流布与 `same_line` 换列、按钮点击往返、`##` 后缀只藏显示不藏 id（同窗口两个 `Save##x` 是两个控件、宽度按可见文本算）、长文本自动折行且不丢字、`spacing(0)` 空操作与 `spacing(<0)` 前进一个主题行间距、`wants_mouse` 跟随鼠标进出窗口而 `wants_keyboard` 跟随输入焦点、色块打包 RGBA、Progress 填充、Label 字符串拼装、两个分隔符是两个槽、绘制序为 chrome 先于内容且内容被窗口裁剪、同标签不同窗口互相独立、`last_item_*` 查询。

`GryceUiSmoke.exe` 是模型层的端到端演示（不进门禁，需窗口）：面板画出来、`--ops-burst N` 灌一批操作后统计里 `coalesced` 应接近连续项条数、`--ui-interval 3` 与每帧构建的截图逐字节一致。Layout 页（`--page layout` / `Ctrl+3`）画一棵 20 个组件的树，每帧 `GUi_CompResolve()` 后逐组件取矩形画窗口——组件窗口**不传** `GUI_WIN_NO_BRING_TO_FRONT`，否则子窗口会被压到父窗口底下（原因见 ui-api.md 第 7.7 节）。Anim 页（`--page anim` / `Ctrl+4`）面板里是块一纯函数演示（缓动组合框 + 补间进度条 + 颜色插值色块），右侧舞台是块二：一根 2 秒片段驱动四个组件（滑移/脉冲/弹跳/错峰入场），这些组件父级为 0，动画直写 pos 不会被布局覆盖。加 `--ui-backend builtin` 则整场 UI 走内置渲染路径（不吃 ImGui 上下文），逐页截图应与 ImGui 后端视觉等价——这条是"双轨可切换"的唯一端到端验证手段，因为它覆盖了单元测试碰不到的 GL/D3D 状态与字体图集。

## 3. 执行注意

`grycegc_*` 用例并行执行会互相干扰（共享临时目录与进程级状态），本地全量验证请串行跑：

```bash
ctest --test-dir build -j1 --output-on-failure
```

CI 上各用例独立注册、由 runner 调度，不受此限。

## 4. 新增用例

- 数学 / 资源规则套件：在根 `CMakeLists.txt` 的 `GRYCE_MATH_TEST_SUITES` 里加名字，自动注册成一条 CTest。
- 资产 / 编辑器 / UI 套件：分别在 `GRYCE_ASSET_TEST_SUITES`、`GRYCE_EDITOR_TEST_SUITES`、`GRYCE_UI_TEST_SUITES` 里加名字。新增编辑器用例时注意两种返回约定（入队 1/0、查询与场景操作 0/-1），写错会让断言永远为真的假绿。
- 工具链套件：在 `toolchain/CMakeLists.txt` 的 `_grycegc_suites` 里加名字。

## 5. 历史：像素比对用例已删除

早期的 `renderer_smoke_opengl` / `renderer_smoke_vulkan` 两条像素用例及其 Python 工具已删除：它们靠"截图里非背景像素占比"判定，受无头环境 / 软件渲染 / 驱动版本影响很大，既不反映构建正确性也不反映计算正确性。`GryceRenderSmoke.exe` 本身保留为人工运行的演示宿主；Web 端同理不进 CTest，只做人工验证（见 [webgl-backend.md](webgl-backend.md) 第 7 节）。

## 6. CI 配置

`.github/workflows/ci.yml`：任意 push / PR 触发，Windows / MSYS2 UCRT64，Release 配置，安装 GCC / CMake / Ninja / GLFW / GLEW / Box2D / Jolt 系统包（不装 Assimp 与 Vulkan SDK，对应路径自动降级），然后 configure + build + CTest。
