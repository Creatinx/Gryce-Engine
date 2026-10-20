# 编辑器接口（editor-api）

本文汇总**编辑器（外部进程）与引擎之间**的接口面：编辑命令、撤销/重做、视口输出与 ImGui 桥、双视口、变换 Gizmo、显示模式、资产侧车文件。这些接口都是 C ABI，纯头文件即可接入，不依赖引擎内部 C++ 类型。

引擎侧不再需要为编辑器新增底层能力：本文件描述的是**已经就绪**的前置条件。

界面控件（`GUi_*`）单独一篇：[ui-api.md](ui-api.md)。

## 1. 编辑命令面（`api/edit_api.h`）

所有编辑操作走同一条通道：`GCore_PushCommand` → 命令缓冲 → `GCore_BeginFrame` 内统一消费（每帧上限 512 条、4 ms 预算，未处理的下帧重入队）。编辑器因此不需要（也不应该）直接改场景。

```c
// 入队成功返回 1，失败 0 —— 返回值只表示"是否入队"，不代表执行结果。
GEdit_CreateEntity("Wall", 0);          // 0 = 场景根
GEdit_SetTransform(handle, &pos, NULL, NULL);
```

`edit_api.h` 同时公开每条命令的 payload 结构体（`GCmdCreateEntity` … `GCmdGizmoManipulate`），带 `static_assert(sizeof(...) <= 256)` 编译期守卫。此前这些结构体只写在 `core_api.cpp` 内部，编辑器只能照抄字节布局，引擎一改字段就静默读错。

| 命令 | 封装函数 | 说明 |
| --- | --- | --- |
| `ECMD_CREATE_ENTITY` | `GEdit_CreateEntity(name, parent)` | 创建并**自动选中**（`GEntity_GetSelected` 可取到新句柄） |
| `ECMD_DESTROY_ENTITY` | `GEdit_DestroyEntity(e)` | 连子树一起销毁 |
| `ECMD_RENAME_ENTITY` | `GEdit_RenameEntity(e, name)` | |
| `ECMD_REPARENT_ENTITY` | `GEdit_ReparentEntity(e, parent)` | 安全重挂，不会销毁被挂实体 |
| `ECMD_SELECT_ENTITY` | `GEdit_SelectEntity(e)` | 触发 `OnEntitySelected` 回调 |
| `ECMD_SET_TRANSFORM` | `GEdit_SetTransform(e, pos, rot, scale)` | 传 `NULL` 的分量**用当前值补齐**；`rot` 全 0 视为保持原旋转 |
| `ECMD_TRANSLATE_ENTITY` | `GEdit_TranslateEntity(e, delta)` | 局部增量平移，**执行时**才读当前值，同帧多条可叠加 |
| `ECMD_SET_PROPERTY` | `GEdit_SetProperty(e, type_hash, name, value, size)` | `value` 是反射字段的原始字节 |
| `ECMD_ADD_COMPONENT` | `GEdit_AddComponent(e, type_name)` | 类型全名 |
| `ECMD_REMOVE_COMPONENT` | `GEdit_RemoveComponent(e, type_hash)` | |
| `ECMD_SET_MATERIAL` | `GEdit_SetMaterial(e, mesh, r, g, b, rough, metal)` | 无 `MeshRenderer` 时自动补 |
| `ECMD_SET_RENDER_TARGET` | `GEdit_SetRenderTargetSize(target, w, h)` | `target`：0 = SceneView，1 = GameView |
| `ECMD_SAVE_SCENE` | `GEdit_SaveScene(path)` | |
| `ECMD_IMPORT_ASSET` | `GEdit_ImportAsset(path)` | 建实体 + `MeshRenderer` + 选中 |
| `ECMD_PLAY_MODE` | `GEdit_SetPlayMode(mode)` | 0 = Stop，1 = Play，2 = 暂停切换 |
| `ECMD_STEP_FRAME` | `GEdit_StepFrame(frames)` | 仅在 Play 且暂停时前进指定帧数 |

字符串字段一律定长数组，超长截断并保证 `'\0'` 结尾。

### 同一帧多条变换必须用增量版

`GEdit_SetTransform` 在**入队时**就用当前值补齐缺省分量，所以同一帧里先
`SetTransform(pos1)` 再 `Translate(delta)` 时，第二条读到的还是旧位置，算出的
绝对值会把第一条覆盖掉——最终只生效一次，而且看不出错。

```c
// 错：最终位置是 old + delta，pos1 丢了
GEdit_SetTransform(e, &pos1, NULL, NULL);
GEdit_TranslateEntity(e, &delta);        // 若这里改成 SetTransform(old+delta) 就错了

// 对：增量命令在执行时才读位置，两条都在下一帧按顺序叠加
GEdit_SetTransform(e, &pos1, NULL, NULL);
GEdit_TranslateEntity(e, &delta);        // ⇒ pos1 + delta
```

Inspector 连续改两个数值框、Gizmo 拖拽后紧跟一次吸附修正，都会踩这个坑。

### 返回值约定（两种，别混用）

| 类别 | 约定 | 例子 |
| --- | --- | --- |
| 命令入队（`GEdit_*`） | 1 = 已入队，0 = 参数非法/队列满 | `GEdit_CreateEntity` |
| 查询与场景操作（`GEntity_Get*`、`GScene_*`） | 0 = 成功，-1 = 失败 | `GEntity_GetLocalPosition`、`GScene_Save` |

`GEntity_GetName` 是例外：返回**名字长度**，判 `len < 0` 才是失败。

## 2. 撤销 / 重做（`GCore_Undo` 系列）

撤销采用**整场 JSON 快照**，不是增量 diff。理由：序列化路径本来就要写盘、正确性有存档路径兜底；恢复后实体句柄按 UUID 保留（`EntityHandleMap::rebuild` 不重置已有映射），编辑器持有的选择与面板状态无需重建。

```c
GCore_Undo();               // 1 = 执行了撤销，0 = 无可撤销
GCore_Redo();
GCore_GetUndoCount();
GCore_GetRedoCount();
GCore_SetUndoLimit(64);     // 0 = 不记录
GCore_ClearUndoHistory();

GCore_BeginEdit("Drag");    // 拖拽类连续编辑：整段只算一步
GCore_EndEdit();
```

配套语义：

* 每条**变更类**命令执行前自动抓快照，`BeginEdit` 期间只抓一次（结束点入栈）。
* Play 模式下撤销栈冻结：播放期的物理/动画改动本来就不该进编辑历史。
  **宿主默认是运行时语义**（启动即进 Play），所以编辑器项目必须在 `project.data`
  里写 `"start_in_play": false`，或启动时加 `--edit-mode`；忘了这一步的表现是
  "所有编辑都能生效，但撤销按钮永远灰的、脏标记永远干净"（见 [app-entry.md](app-entry.md) 第 6 节）。
* `GScene_Load` / `GScene_New` / `cmd_load_scene` 都会清空历史——旧快照里的 UUID 在新场景里不存在，留着只会"撤销出一堆幽灵实体"。
* 栈深默认 64，超出从最早一端丢弃。

## 3. 视口输出与 ImGui 桥（`api/render_api.h`）

编辑器把 3D 画面画进自己的面板，需要三件事：离屏颜色纹理、它的 ImGui 描述符、以及 ImGui 上下文的归属。

```c
GRender_SetViewportOutput(true);                  // 开启离屏输出（init 之前调用也生效）
GTextureHandle tex = GRender_GetViewportTexture();// tonemap 后的 LDR 结果
ImTextureID id = GRender_GetImGuiTextureId(tex);  // 三后端均已实现
GRender_InvalidateImGuiTexture(tex);              // 重建渲染目标后必须调用
```

`GTextureHandle` 就是 `ITexture*`，因此句柄可以跨后端直接喂给 `IImGuiBackend::imgui_texture_id`。

ImGui 上下文归属：引擎只在 `ImGui::GetCurrentContext() == nullptr` 时接管（条件为 `debug_ui_enabled || ui_callback`）。宿主自持上下文时改用回调，让引擎在自己的 ImGui 帧里画 UI：

```c
GRender_SetUICallback(my_draw_ui, user_ptr);
GRender_HasImGui();          // 引擎当前是否真的持有可用上下文
```

回调内不能调 `GRender_*`（锁已持有），能用的是 `api/ui_api.h` 的 `GUi_*`，详见 [ui-api.md](ui-api.md)。

### 编辑器相机：传四元数，别传 yaw/pitch

```c
GViewport_SetEditorCamera(px, py, pz, yaw_deg, pitch_deg, fov, near, far);
GViewport_SetEditorTransform(px, py, pz, qx, qy, qz, qw, fov, near, far);   // 推荐
```

`SetEditorCamera` 的语义是引擎 `math::Camera` 的约定：`forward = (cos yaw, sin pitch, sin yaw)`。而编辑器常用的约定是 `forward = (-sin yaw, sin pitch, -cos yaw)`——两者都能自圆其说，直传的结果是画面与拾取一起偏 90 度，且"偏 90 度"和"pitch 符号反了"在单张截图上几乎分辨不出来。

`GViewport_SetEditorTransform` 用四元数，**旋转作用在相机默认朝向 -Z 上**（与场景 `Camera` 组件、与 `build_scene_camera` 完全同一条推导）。宿主用同一个四元数算拾取射线，就必然与渲染一致，不存在换算式。

```c
// 编辑器里的构造顺序：先偏航后俯仰
Quat q = quat_mul(quat_axis_angle(0, 1, 0, yaw), quat_axis_angle(1, 0, 0, pitch));
GViewport_SetEditorTransform(pos, q.x, q.y, q.z, q.w, fov, 0.1f, 1000.0f);

// 拾取射线用同一个 q 旋转 -Z，不要再从 yaw/pitch 反算
Vec3 forward = rotate_by_quat(q, { 0, 0, -1 });
```

## 4. 双视口（SceneView / GameView）

两张画面共用同一条 HDR / MSAA / 后处理链——场景只渲染一次，`tonemap` 时按 `set_game_view_active` 决定写哪份目标。收益是不必再把整条链复制一份，代价是 **GameView 的内部分辨率跟随 SceneView**（尺寸不一致会被拒绝并记日志）。

```c
GRender_SetGameViewCamera(camera_entity);   // 未设置时回退到场景主相机
GRender_RenderGameView();                   // tonemap 写 GameView 目标
GTextureHandle game = GRender_GetGameViewTexture();
```

### 建议帧序

```
PollEvents → 输入同步 → GCore_BeginFrame(dt)
          → GRender_BeginFrame()
          → GRender_RenderWorld()        // SceneView（编辑器相机）
          → GRender_RenderGameView()     // GameView（场景相机）
          → GRender_RenderGizmo()        // 实际绘制在 ImGui 帧里
          → GRender_EndFrame()
          → GCore_EndFrame()
```

## 5. 变换 Gizmo

Gizmo 用 ImGui foreground draw list 做**屏幕空间**绘制，不进 3D 管线：三后端零成本、天然盖在物体之上、不需要深度遮挡。

```c
GRender_SetGizmoEnabled(true);
GRender_SetGizmoMode(0);            // 0 = 世界空间，1 = 局部
GRender_SetGizmoOperation(0);       // 0 = 平移，1 = 旋转，2 = 缩放

int axis = GRender_GizmoHitTest(mouse_x, mouse_y);   // ≥0 = 命中轴，-1 = 未命中
GRender_BeginDrag(axis, mouse_x, mouse_y);
GRender_Drag(mouse_x, mouse_y);     // 每帧调用，内部换算成世界增量
GRender_EndDrag();

GRender_DrawGizmoOverlay();         // 引擎接管 ImGui 时由 EndFrame 自动调用
```

拖拽增量最终应经 `GCore_BeginEdit` + `GEdit_SetTransform` 落到场景（而不是直接改 Transform），否则撤销栈看不到这次修改。

命中半径 9 px、轴长 92 px；轴色红/绿/蓝，悬停与拖拽中的轴高亮为黄。

### 视口不是全窗口时必须设屏幕原点

Gizmo 画在 ImGui 的前景 draw list 上，那是**窗口绝对坐标**；而视口贴图只知道自己是 `vw × vh`。所以 `screen_project` 默认把 NDC 映射到 `[0,vw] × [0,vh]`——视口贴着面板左上角时，gizmo 会画在窗口左上角、浮在层级树上面。

```c
GViewport_SetSize((int)w, (int)h);   // 视口内部尺寸：NDC → 像素的除数
GViewport_SetScreenOrigin(x, y);     // 视口在窗口里的左上角：像素偏移
GViewport_GetScreenOrigin(&x, &y);
```

契约：

| 接口 | 入参坐标系 |
| --- | --- |
| `GRender_GizmoHitTest(mx, my)` | **窗口绝对坐标**（设过原点后） |
| `GRender_BeginDrag` / `GRender_Drag(mx, my)` | 同上 |
| `GScene_PickRay(ox, oy, ...)` / 编辑器自算的 `screen_ray` | **视口内坐标** = `mouse_abs - origin` |

不设原点时原点为 `(0,0)`，等价于"视口铺满窗口"，与旧行为完全一致——这是有意的默认值，纯 3D 演示程序不用改代码。

原点应在每帧 `tick` 里按**实际绘制矩形**同步（`GUi_LastItemRect`），不要用请求的尺寸：窗口缩放时请求值和实际值会差一两个像素，gizmo 会缓慢漂移，表现为"鼠标越靠右下，拾取偏得越多"。


## 6. 显示模式

```c
GRender_SetDisplayMode("Wireframe");
GRender_IsDisplayModeSupported("Wireframe");   // 1 = 当前后端能真正渲染
```

线框只作用于**几何 pass**（GBuffer / 主前向 pass），不作用于全屏后处理——后处理的四边形式全屏三角形一经线框化，整屏只剩两条对角线。

| 后端 | Wireframe |
| --- | --- |
| OpenGL（桌面） | 支持（`glPolygonMode`） |
| WebGL | 不支持（GLES3 无此入口） |
| Vulkan | 不支持（需 `VK_EXT_extended_dynamic_state3`） |
| D3D12 | 不支持（需为每个着色器准备 wireframe 版 PSO） |

`GRender_IsDisplayModeSupported` 就是让编辑器**禁用**菜单项用的，避免用户点了个什么都不发生的开关。

## 7. 资产侧车文件（`<asset>.meta`）

编辑器需要"资产的身份"独立于路径：层级引用、材质槽、场景片段都按 `guid` 指向资产，重命名/移动不断链。侧车同时保存导入设置，下次导入按它生效。

```c
GAssetMetaInfo info;
GAsset_EnsureMeta("res:/models/wall.fbx", &info);   // 缺失则生成并写盘
info.import_materials = 0;
GAsset_WriteMeta("res:/models/wall.fbx", &info);    // 写盘并让旧网格缓存失效
GAsset_IsMetaStale("res:/models/wall.fbx");         // 源文件被替换后提示重新导入
```

侧车路径为 `<源文件名>.meta`（保留原扩展名，避免 `texture.png.meta` 与 `texture.meta` 互相覆盖），JSON，UTF-8。字段：

| 字段 | 默认 | 作用 |
| --- | --- | --- |
| `guid` | 生成 | 32 位十六进制，首次导入生成后永不改变 |
| `importer` | `mesh` | 决定哪些设置生效 |
| `scale` | 1.0 | 源文件单位 → 米（厘米导出的摄影测量模型填 0.01） |
| `up_axis` | 1 | 0 = X-up，1 = Y-up，2 = Z-up（Z-up 绕 X 轴 -90° 转 Y-up） |
| `generate_lod` | true | 导入后是否生成 LOD 链 |
| `import_materials` | true | false = 白模，忽略文件自带材质 |
| `roughness_scale` | 1.0 | 叠在材质推导结果上的倍率 |
| `source_mtime_ns` / `source_size` | 记录 | 源文件指纹，用于判定过期 |

实现要点：

* 缺失字段取默认值、未知字段忽略——侧车可能由更新版本的编辑器写出，旧引擎读到不能报错。
* 非法设置（`scale <= 0`、`up_axis` 越界）回退默认值，但 **guid 保留**：身份不能被设置项带坏。
* 目录只读或资产来自 `.gpack` 时不写盘（只返回内存结果），属于正常降级。
* 缩放与上轴由 `apply_import_options` 统一处理，内置 OBJ 加载器与 Assimp 路径共用同一份实现——同一工程里 OBJ 与 FBX 才不会错位。
* `GRYCE_NO_ASSET_META=1` 全局关闭（纯运行时构建、只读资源目录）。

### 目录列举（`GAsset_ScanDirectory`）

资产浏览器要列目录，但打包清单、热更校验、CI 资源检查同样需要，所以这是引擎能力而不是编辑器私货：

```c
char buf[8192];
int n = GAsset_ScanDirectory("res:/models", /*recursive=*/0, buf, sizeof(buf));
if (n >= (int)sizeof(buf)) {            // 缓冲不够：n 是所需长度，扩容重来
    std::vector<char> big(n + 1);
    GAsset_ScanDirectory("res:/models", 0, big.data(), n + 1);
}
```

输出单行 JSON：`{"dir","count","truncated","items":[{"path","name","ext","size","guid","has_meta","stale","kind"}]}`。

* 只做文件系统遍历，**不加载**任何数据——逐个路径试 `GAsset_Load` 既慢又区分不出"文件不存在"和"格式不支持"。
* `.meta` 侧车不算资产，自动排除。
* 结果按 `path` 字典序，保证同一目录每次列举顺序一致（界面不抖动）。
* `kind`（mesh / texture / audio / scene / script / shader / other）只用于列表分组，不代表能否成功导入。
* 返回值语义：写入字节数；缓冲不足时返回**所需**长度（大于 `buf_size`），失败 -1。

### 访客式遍历（`GAsset_ScanDirectoryEach`）

资产面板既要用 JSON（脚本侧、`panel_assets_json()`），又要直接读结构体（UI 每帧都要用，解析自己刚生成的字符串既慢又多一种"解析失败 → 列表空白"的失败模式）。两个版本共用同一份实现，遍历顺序、排除规则、侧车字段完全一致：

```c
typedef struct GAssetEntry {
    const char* path;  const char* name;  const char* ext;
    const char* kind;  const char* guid;
    unsigned long long size;
    int has_meta;      int stale;
} GAssetEntry;

// visitor 返回非 0 则提前中止；函数返回访问到的条目数
int cb(void* user, const GAssetEntry* e) { /* push to vector */ return 1; }
GAsset_ScanDirectoryEach("res:/models", /*recursive=*/0, cb, &my_vector);
```

`GAssetEntry` 里的字符串指针**只在回调期间有效**（指向遍历用的临时缓冲），要留存必须自己拷。这是为了避免每个条目都做一次堆分配——目录里几千个文件时差别明显。

## 8. 已知边界

* 撤销是整场快照：单步开销随场景规模增长（demo 场景 1.4 M 三角形约几百微秒），栈深不要开太大。
* GameView 分辨率跟随 SceneView，需要独立分辨率就得复制整条 HDR 链。
* 线框仅 OpenGL 桌面可用；其余后端如实回报不支持。
* Gizmo 只画轴与量具，不做多选合并、不做吸附（吸附由编辑器在 `GEdit_SetTransform` 前自行取整）。
* `screen_project` 只做透视投影 + 原点偏移，不认识 ImGui 的裁剪矩形：视口被滚动或部分遮住时，gizmo 仍会画在矩形外，裁剪靠 ImGui 的 clip rect 兜。
* 界面本身（`GUi_*`）见 [ui-api.md](ui-api.md)：无 docking、单套暗色主题、中文字形依赖引擎已加载的字体图集。
