// ===========================================================================
// editor/ext/ext.h — 编辑器能力声明
//
// 本文件是 ext 的**唯一事实源**：编辑器需要什么能力，全在这里声明；实现散在
// 各子目录，`ext.cpp` 只做装配，`bridge/js_register.cpp` 只做 JS 绑定。
//
// 归属标注（每一条都必须有，新增能力时照抄格式）：
//   @owner engine    引擎该做，直接改 engine/api/，本文件不声明
//   @owner wrapper   引擎有原语，ext 补编辑器语义（多选、增量、分组、换算）
//   @owner ext       纯编辑器概念，引擎没有必要知道
//
// 本文件**不声明** @owner engine 的能力 —— 它们根本不进 ext。一条能力一旦判定
// 属于引擎，就加到 engine/api/ 里，而不是在 ext 用 C ABI 拼一遍。判定规则见
// ../README.md 第 2 节，判定结果见第 3 节的归属表。
//
// 返回约定（写实现前先看这段，混用是最常见的 bug 来源）：
//   - 入队类命令（create/destroy/rename/set_*/add_*/remove_*/save/import）
//     返回 1 = 已入队，0 = 参数被拒。**入队不等于已生效**，下一帧
//     GCore_BeginFrame 才执行。
//   - 即时查询类（select/pick/undo/redo/gizmo_hit_test/...）返回 1/0 或数值。
//   - "参数被拒"分两种：语义非法（句柄为负、NaN、名字为空、缩放为 0）返回 0；
//     参数类型完全不对（该给数字给了字符串）由 JS 桥抛错。调用方对前者用
//     返回值判断，对后者不必处理 —— 类型错了本来就是调用方的 bug。
//   - 所有 `const char*` 返回值指向 ext 内部的轮转缓冲（ext.cpp 的 `hold()`）：
//     当帧内有效，调用方要留就自己拷。缓冲轮转 8 份，所以一次拼 JSON 里调用
//     多次子函数也不会互相覆盖。
//
// 命名空间约束：扩展 ABI 的 add_function 只能挂 `engine.<ns>` 一级，因此 JS 侧
// 全部平铺在 `engine.editor.*` 下，用下划线分组（view_* / gizmo_* / panel_*），
// 不做 `engine.editor.view.xxx` 这样的二级命名空间。
// ===========================================================================

#ifndef GRYCE_EDITOR_EXT_H
#define GRYCE_EDITOR_EXT_H

#include "api/asset_api.h"
#include "api/types.h"

#include <cstdint>

namespace gryce::editor {

// ===========================================================================
// 生命周期
// ===========================================================================

// 首次装配：清空会话、选择集、历史分组状态。可重入（== 重新打开文档）。
// @owner ext
void init();

// 退出前收尾：结束时仍打开的历史分组、释放会话占用。
// @owner ext
void shutdown();

// 每帧驱动，由 main.js 显式调用（扩展不注册帧回调，见 ../README.md 第 6 节）。
// 职责：推进待确认命令、判定脏标记、失效面板缓存、同步视口尺寸。
// @owner ext
void tick(float dt);

// 取走本帧事件（取走即清空，避免同一个事件被重复上报）。
// ["created:12","deleted","create_unconfirmed"]
// @owner ext
const char* pending_events_json();

// "editor-ext 1.0 (gryce)" —— JS 侧据此判断扩展在不在。
// @owner ext
const char* version();

// ===========================================================================
// 会话：当前文档、脏标记、统计
// ===========================================================================

// 打开文档。path 为 res:/ 形式或真实路径；成功入队返回 1。
// 换文档会清空选择集与编辑历史（旧快照里的 UUID 在新场景里不存在）。
// @owner ext
int open(const char* path);

// 关闭当前文档：清空选择集、清空历史、但不写盘。
// @owner ext
void close();

// 保存到 path；path 为空或空串时用当前文档路径。返回 1 = 已入队。
// @owner wrapper  —— GEdit_SaveScene + "保存后刷新脏标记基线"
int save(const char* path);

// {"path":"res:/scenes/main.gesc","name":"main.gesc","dirty":0,
//  "entities":7,"playing":0,"paused":0,"selected":3,"selectionCount":1}
// @owner ext
const char* session_json();

// ===========================================================================
// 选择
//
// 引擎是单选（GEdit_SelectEntity / GEntity_GetSelected）。编辑器要的多选、
// 主选中与附选的区分、按集合删除，都在这里补。
// ===========================================================================

// 设为唯一选中。handle 传 0 表示清空选择。
// @owner wrapper
int select(GEntityHandle handle);

// 追加到选择集（不改变主选中）。
// @owner ext
int select_add(GEntityHandle handle);

// 切换（已选则取消，未选则追加）。
// @owner ext
int select_toggle(GEntityHandle handle);

// 从选择集移除；移除的是主选中时，主选中退到集合里第一个。
// @owner ext
int select_remove(GEntityHandle handle);

// @owner ext
void select_clear();

// {"primary":3,"count":2,"items":[3,6]}
// @owner ext
const char* selection_json();

// 删除选择集里的全部实体，作为**一次**撤销步骤（分组）。
// @owner wrapper  —— GCore_BeginEdit/EndEdit + 逐个 GEdit_DestroyEntity
int delete_selection();

// 屏幕坐标拾取（视口左上角为原点）。未命中返回 0。
// @owner wrapper  —— view::screen_ray + GScene_PickRay
//   刻意不用 GScene_PickScreen：那个接口打的是场景里的 Camera 实体，而编辑器
//   视口的朝向来自 GViewport_SetEditorTransform，两者可以完全无关，用错的表现是
//   "点左边选中右边"。
GEntityHandle pick(float sx, float sy);

// 选中实体的中心点，供"聚焦"用。引擎还没暴露包围盒查询，暂用
// "世界位置 + 直接子节点世界位置"的平均值代替；换真包围盒时要按 README
// 规则一先给引擎加 GEntity_GetWorldBounds。
// {"ok":1,"x":..,"y":..,"z":..} 或 {"ok":0}
// @owner wrapper  —— GEntity_GetWorldPosition + GEntity_GetChildAt
const char* focus_json();

// ===========================================================================
// 编辑命令
//
// 全部转 GEdit_*。参数校验在 ext 做（例如 NaN、句柄 <= 0、名字为空），因为
// 入队接口没有返回值通道，错了只能靠日志追。
// ===========================================================================

// 创建实体；parent 传 0 表示挂在场景根。返回 1 = 已入队。
// 新实体的句柄在下一帧通过"创建即选中"约定确认，见 command/commands.cpp。
// @owner wrapper
int create(const char* name, GEntityHandle parent);

// @owner wrapper
int destroy(GEntityHandle handle);

// @owner wrapper
int rename(GEntityHandle handle, const char* name);

// 重新挂父；parent 传 0 表示挂回场景根。会拒绝"挂到自己的子树下"。
// @owner wrapper
int reparent(GEntityHandle handle, GEntityHandle parent);

// 三个分量接口各自只改一个分量，其余保持当前值 —— 对应
// GEdit_SetTransform 的"传 nullptr 保持原值"语义，Inspector 的单数值框
// 与 Gizmo 的单轴拖拽都依赖这点。
// @owner wrapper
int set_position(GEntityHandle handle, float x, float y, float z);
// @owner wrapper
int set_rotation(GEntityHandle handle, float x, float y, float z, float w);
// @owner wrapper
int set_scale(GEntityHandle handle, float x, float y, float z);

// 增量平移（相对当前值）。
// @owner wrapper
int translate(GEntityHandle handle, float dx, float dy, float dz);

// 反射属性写入。type_hash 是组件类型哈希（用 add_component 的 type_name
// 从引擎注册表查），prop_name 是反射属性名。按属性实际类型写入。
// @owner wrapper
int set_property_number(GEntityHandle handle, uint64_t type_hash,
                        const char* prop, double value);

// @owner wrapper
int set_property_bool(GEntityHandle handle, uint64_t type_hash,
                      const char* prop, int value);

// 向量 / 四元数 / 颜色属性。count 必须等于字段的有效分量数
// （vec2=2、vec3/vec3i=3、vec4/quat/color=4），与字段类型不符时返回 0，
// 不会只写一半留下"改了但没变"的状态。
// @owner wrapper
int set_property_vec(GEntityHandle handle, uint64_t type_hash,
                     const char* prop, const float* values, int count);

// 字符串属性（资源路径这类）。字段不是字符串时返回 0。
// @owner wrapper
int set_property_string(GEntityHandle handle, uint64_t type_hash,
                        const char* prop, const char* value);

// 组件增删。type_name 用引擎注册表里的名字（"Light" / "Camera" / ...）。
// remove 用 type_hash（名字已经拿到过就没必要再查一次表）。
// @owner wrapper
int add_component(GEntityHandle handle, const char* type_name);
// @owner wrapper
int remove_component(GEntityHandle handle, uint64_t type_hash);

// 网格与材质一体设置。mesh_path 传空串表示只改材质。
// @owner wrapper
int set_material(GEntityHandle handle, const char* mesh_path,
                 float r, float g, float b, float roughness, float metallic);

// 导入资产并实例化：建实体 + MeshRenderer，挂到场景根。
// @owner wrapper
int import_asset(const char* path);

// ===========================================================================
// 历史
//
// 引擎已经实现整场 JSON 快照式撤销栈（GCore_Undo/Redo/BeginEdit/EndEdit）。
// ext 只补两件事：一次用户操作合并成一步的策略、以及"撤销后选择集要修"。
// ===========================================================================

// @owner engine 转调 —— 返回 1 = 执行了撤销
int undo();

// @owner engine
int redo();

// {"undo":3,"redo":0,"canUndo":1,"canRedo":0,"group":""}
// @owner wrapper  —— 引擎只给计数，分组标签是 ext 的
const char* history_json();

// 显式分组。一次 Gizmo 拖拽、一次多选删除都应该只用一步撤销。
// 嵌套调用按最外层计（引擎侧只有一个 group_open 标志）。
// @owner wrapper
void begin_edit(const char* label);
void end_edit();

// @owner engine
void clear_history();

// ===========================================================================
// 播放模式
// ===========================================================================

// @owner wrapper
int set_play(int play);

// @owner wrapper
int toggle_play();

// 单步：暂停态下前进一帧。
// @owner wrapper
int step_frame();

// ===========================================================================
// 视口
//
// 编辑器用离屏视口（场景渲进纹理），窗口留给 UI。关闭时回到"直接写窗口"的
// 运行时形态，用于快速对比渲染结果。
// ===========================================================================

// @owner engine 转调 GRender_SetViewportOutput
int view_enable_output(int enabled);

// 请求视口尺寸。走 GEdit_SetRenderTargetSize 入队，下一帧由 renderer 取走。
// @owner wrapper
int view_set_size(int w, int h);

// 编辑器相机（不依赖场景里的 Camera 实体）。
// @owner engine
void view_set_editor_camera(float x, float y, float z,
                            float yaw_deg, float pitch_deg, float fov_deg);

// {"width":1600,"height":900,"output":1,"uiBackend":"builtin",
//  "gizmo":1,"gameView":1,"displayMode":"Shaded"}
// @owner ext
const char* view_info_json();

// 显示模式："Shaded" / "Wireframe"。不支持的后端返回 0 且不改变状态。
// @owner wrapper  —— GRender_SetDisplayMode + 后端能力查询
int view_set_display_mode(const char* mode);

// @owner engine 转调 GRender_IsDisplayModeSupported
int view_display_mode_supported(const char* mode);

// ===========================================================================
// Gizmo
//
// 引擎已经实现屏幕空间绘制、命中测试、像素→变换换算与像素拖拽
// （GRender_Gizmo*），拖拽期间的 BeginEdit/EndEdit 也在引擎侧成对下发。
// ext 补的是"手势状态"这一段：悬停轴记忆、防止重复 Begin 顶掉分组标志、
// 单帧位移夹取、以及给 UI 读的模式/进度快照。
// ===========================================================================

// @owner engine 转调 GRender_SetGizmoEnabled
int gizmo_enable(int enabled);

// operation: 0=平移 1=旋转 2=缩放；space: 0=世界 1=局部。
// @owner engine 转调 GRender_SetGizmoMode
int gizmo_set_mode(int operation, int space);

// {"operation":0,"space":0,"enabled":1,"axis":-1}
// @owner wrapper
const char* gizmo_mode_json();

// 命中轴：0=X 1=Y 2=Z，-1 = 未命中。
// @owner engine 转调 GRender_GizmoHitTest
int gizmo_hit_test(float sx, float sy);

// 开始拖拽（axis 来自 hit_test）。会 GCore_BeginEdit("gizmo")。
// @owner wrapper
int gizmo_begin_drag(int axis);

// 逐帧拖拽。delta_px 是屏幕像素增量（横向+纵向合并）。
// 换算与写回变换都在引擎里（GRender_GizmoDrag），ext 只夹住单帧位移上限。
// @owner wrapper
int gizmo_drag(float delta_px);

// 结束拖拽：GCore_EndEdit()，刷新脏标记。
// @owner wrapper
int gizmo_end_drag();

// ===========================================================================
// 资产
// ===========================================================================

// 目录列举，返回引擎给的 JSON 原样（见 GAsset_ScanDirectory）。
// 引擎做过一次搬运：这个接口在 ext 里只是转发，不做任何加工 ——
// 加工会让"引擎看到的资产"与"编辑器看到的资产"产生差异。
// @owner engine 转调 GAsset_ScanDirectory
const char* asset_scan(const char* dir, int recursive);

// @owner engine 转调 GAsset_Import
GAssetHandle asset_import(const char* path);

// 侧车信息转成前端好用的 JSON。
// {"ok":1,"guid":"...","importer":"mesh","scale":1,"upAxis":1,
//  "generateLod":0,"importMaterials":1,"roughnessScale":1,"stale":0}
// @owner wrapper
const char* asset_meta_json(const char* path);

// ===========================================================================
// 面板
//
// ext 只产出**数据**，不画界面。这样 ext 不需要 imgui.h，也不会被 ImGui
// 版本绑死；换 UI 框架只改渲染侧，扩展不动。
// ===========================================================================

// 场景层级树：{"ok":1,"roots":[{"handle":1,"name":"A","children":[...]}]}
// @owner ext
const char* panel_hierarchy_json();

// 实体检视：{"ok":1,"handle":3,"name":"Cube","parent":0,
//  "transform":{"position":[..],"rotation":[..],"scale":[..]},
//  "components":[{"name":"MeshRenderer","hash":"...","properties":[
//     {"name":"mesh_path","type":3,"value":"res:/models/cube.obj"}]}]}
// @owner ext
const char* panel_inspector_json(GEntityHandle handle);

// 资产浏览：引擎列举 + ext 按 kind 分组。
// @owner wrapper
const char* panel_assets_json(const char* dir);

// 一帧的统计：{"entities":7,"selected":3,"undo":2,"redo":0,
//  "dirty":0,"fps":60.1,"tris":1234}
// @owner ext
const char* panel_stats_json();

// ===========================================================================
// 快捷键
//
// 键位表放在 ext 而不是 main.js：同一份表既能被 UI 读去做键位提示，也能被
// 冲突检测用；写在 JS 里就只能靠人肉保证不重复。
// ===========================================================================

// mods 位掩码：1 = Ctrl，2 = Shift，4 = Alt。
// 重复绑定同名动作视为改键（不是新增），返回 1。
// @owner ext
int shortcut_bind(const char* name, int key, int mods);

// 本帧触发的动作名数组：["undo","translate"]。没有触发返回 "[]"。
// @owner ext
const char* shortcut_poll_json();

// ===========================================================================
// 界面
//
// 界面不是"给脚本用的能力"，而是编辑器自己的实现（ui/ 目录）：它只读面板
// 数据模型、只入队意图。脚本能问的只有三件事 —— 界面开着没有、宿主到底
// 画了没有、当前状态是什么。逐控件驱动 UI 没有意义，也不该给脚本这个权力。
// ===========================================================================

// 界面是否可用：UI 后端是内置轨且渲染器已 Init（回调已注册）。
// 不再取决于宿主有没有 ImGui —— 编辑器整条界面跑在引擎内置轨上。
// @owner ext
int ui_available();

// 开关界面。关掉时会把引擎的 UI 回调一并摘掉（省掉每帧的 UI 求值与绘制）。
// @owner ext
int ui_set_enabled(int enabled);

// 回调被调用的帧数 / 其中真的画出内容的帧数（GUi_InFrame() 为真）。测试用它们确认"宿主确实
// 在渲染界面"，而不是只看注册成功。
// @owner ext
int ui_frame_count();

// 指针是否在视口矩形内。脚本侧的右键转视角只在视口内生效，否则在层级树
// 上右键也会把相机转走。
// @owner ext
int ui_pointer_in_view();

// {"ok":1,"enabled":1,"uiBackend":"builtin","uiReady":1,"frames":120,"draws":118,
//  "viewportTex":"...","viewWidth":900,"viewHeight":640,
//  "dragging":0,"gizmoOperation":0,"gizmoSpace":0,"displayMode":"Shaded"}
// @owner ext
const char* ui_state_json();

} // namespace gryce::editor

#endif // GRYCE_EDITOR_EXT_H
