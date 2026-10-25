// ===========================================================================
// engine/api/ui_api.h — 引擎内置 UI（Dear ImGui）的 C 接口
//
// 引擎已经把 ImGui 编进 GryceRenderer.dll，并在 GRender_SetUICallback 的
// 情况下自己持有 ImGui 上下文（见 render_api.h 的「编辑器 UI 托管」）。本文件
// 把那些控件包成纯 C：宿主/扩展只调用 GUi_*，不需要 include imgui.h，也就
// 不会被 ImGui 版本、临时分配器、DLL 边界上的全局变量绑死。
//
// 为什么必须走 C 接口而不是让扩展直接用 ImGui：
//   ImGui 的上下文、字体图集、临时缓冲都是**进程内全局**且按 DLL 划分。
//   ImGui 编在 GryceRenderer.dll 里，扩展再 include imgui.h 会链接到自己的
//   一份空实现（或必须把 imgui.cpp 也编进扩展），两者的 g 上下文互不相干，
//   表现是"画了但什么都没出现"，且编译期不报错。
//
// 调用时机：**只在 GRender_SetUICallback 注册的回调内有效**。回调运行在
// NewFrame() 与 Render() 之间、且渲染器的锁已持有，因此：
//   * 回调内不得调用 GRender_* / GCore_* / GEdit_* 这类会加锁的公开 API；
//   * 本文件所有函数在回调外调用都安全地返回 0 / 无操作（不会崩，也不会
//     往任何地方画东西），便于宿主在无窗口环境（CI）里跑同一份代码。
//
// 布局约定：本接口不提供 dock。工具型界面用"一整块主窗口 + 固定分区"，
// 理由见 docs/zh/ui-api.md：docking 会让布局随 imgui.ini 漂移，截图比对和
// 脚本断言就不可复现了。
// ===========================================================================

#ifndef GRYCE_UI_API_H
#define GRYCE_UI_API_H

#include <stdint.h>

#ifdef _WIN32
    #ifdef GRYCE_RENDERER_BUILDING
        #define GRYCE_UI_API __declspec(dllexport)
    #else
        #define GRYCE_UI_API __declspec(dllimport)
    #endif
#else
    #define GRYCE_UI_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// 常量（与 ImGui 的枚举解耦：这里是我们自己的编号，内部再做映射）
// ---------------------------------------------------------------------------

// GUi_BeginWindow 的 flags 位
enum {
    GUI_WIN_NONE              = 0,
    GUI_WIN_NO_TITLE_BAR      = 1 << 0,
    GUI_WIN_NO_RESIZE         = 1 << 1,
    GUI_WIN_NO_MOVE           = 1 << 2,
    GUI_WIN_NO_COLLAPSE       = 1 << 3,
    GUI_WIN_NO_SAVED_SETTINGS = 1 << 4,   // 不写 imgui.ini，布局可复现
    GUI_WIN_MENU_BAR          = 1 << 5,
    GUI_WIN_NO_SCROLLBAR      = 1 << 6,
    GUI_WIN_NO_BRING_TO_FRONT = 1 << 7,
    // 内置轨专有：显式要求标题栏（带折叠框，可拖动移窗）。内置轨默认不给
    // 标题栏——引擎里的面板多是"已经排好位置的固定区域"，标题栏会白吃高度。
    // ImGui 轨忽略这一位（它本来就有标题栏，用 NO_TITLE_BAR 关）。
    GUI_WIN_TITLE_BAR         = 1 << 8,
};

// 文本着色（语义化，不暴露主题色值）
enum {
    GUI_TEXT_DEFAULT = 0,
    GUI_TEXT_DIM     = 1,
    GUI_TEXT_OK      = 2,
    GUI_TEXT_WARN    = 3,
    GUI_TEXT_ERROR   = 4,
    GUI_TEXT_ACCENT  = 5,
};

// 鼠标键
enum {
    GUI_MOUSE_LEFT   = 0,
    GUI_MOUSE_RIGHT  = 1,
    GUI_MOUSE_MIDDLE = 2,
};

// 修饰键位（GUi_GetModifiers 的返回值）
enum {
    GUI_MOD_CTRL  = 1 << 0,
    GUI_MOD_SHIFT = 1 << 1,
    GUI_MOD_ALT   = 1 << 2,
    GUI_MOD_SUPER = 1 << 3,
};

// 拖放接受结果的三种状态（GUi_AcceptDragDropPayloadString 的返回值）。
// 区分 PREVIEW 与 DELIVER 是必需的：目标框高亮要在悬停期间每帧都画，
// 而真正的"松手投放"只能执行一次。
enum {
    GUI_DRAG_NONE    = 0,
    GUI_DRAG_PREVIEW = 1,   // 有匹配的 payload 悬停，尚未松手
    GUI_DRAG_DELIVER = 2,   // 本帧松手交付，out_buf 已写入
};

// ---------------------------------------------------------------------------
// 操作队列（UI → 引擎的受控执行通道）
//
// UI 的刷新率不需要跟渲染帧率一样高，所以"UI 想改引擎状态"不当场执行：
// 回调内只入队（纯数据，永远安全），宿主在锁外按策略受控排出。
//
// 为什么需要它而不是在回调里直接调 GEdit_*：
//   * 回调运行在渲染器锁已持有的路径上，当场调引擎 API 会二次加锁死锁；
//   * 拖拽每帧都产生"设变换"，不合并会把一次拖拽变成几十步撤销；
//   * UI 操作批量落地（有预算、有间隔）正好匹配 UI 的低刷新率。
//
// 队列不认识语义：type / key / arg / vec 全部由调用方定义，引擎只保证
// FIFO 顺序、同键合并、预算与统计。key == 0 表示离散操作（按钮点击这类
// 丢了就丢了的语义不存在，绝不合并）；key != 0 表示同 (type,key) 只保留
// 最新值——排队期间连续改动天然塌缩成一条。
// ---------------------------------------------------------------------------

// 一条待执行的 UI 操作。seq 由引擎填写（入队序号，合并不改变），调用方勿改。
typedef struct GUiOp {
    uint32_t type;                          // 调用方自定义的操作码
    uint32_t key;                           // 合并键；0 = 离散操作
    int64_t  arg;                           // 整数参数（句柄 / 枚举 / 布尔）
    float    vec[4];                        // 浮点参数（变换 / 颜色 / 范围）
    uint64_t seq;                           // 入队序号（引擎填）
} GUiOp;

// 入队（回调内调用安全）。返回 1 = 已入队或已合并；0 = 队列满且没有可逐出的
// 旧项（待处理的全是离散操作，一条都不能丢）——调用方应能看见这个失败，
// 而不是被静默吞掉。
GRYCE_UI_API int   GUi_PushOp(const GUiOp* op);

// 排出至多 max 条到 out，返回实际条数。0 = 队列空或未到策略允许的时机
// （后者计入 stats.throttled）。必须在锁外调用——排出来的操作要转成
// GEdit_* / GCore_* 落地，那些调用在回调内是禁止的。
GRYCE_UI_API int   GUi_DrainOps(GUiOp* out, int max);

// 执行策略。max_per_drain <= 0 = 单次不限制；min_frames_between_drain <= 0 =
// 每个推进过的帧都可排。首次排出不受间隔限制。
GRYCE_UI_API void  GUi_SetOpPolicy(int max_per_drain, int min_frames_between_drain);
GRYCE_UI_API void  GUi_GetOpPolicy(int* out_max_per_drain, int* out_min_frames_between_drain);

// 统计快照（任一参数可为 nullptr）。pending 之外都是累计值。
GRYCE_UI_API void  GUi_GetOpQueueStats(int* out_pending,
                                       uint64_t* out_pushed,
                                       uint64_t* out_coalesced,
                                       uint64_t* out_drained,
                                       uint64_t* out_evicted,
                                       uint64_t* out_rejected,
                                       uint64_t* out_throttled);

// 清空待处理操作（换文档 / 重置界面时用；不清累计统计）。
GRYCE_UI_API void  GUi_ClearOps(void);

// 帧计数推进：渲染层每渲染帧调一次，排出间隔以它为基准。自持帧循环的
// 宿主（无窗口 CI）也需要调它，否则带间隔的策略永远不放行。
GRYCE_UI_API void  GUi_AdvanceOpFrame(void);

// ---------------------------------------------------------------------------
// 稳定 id 与跨帧控件状态
//
// ImGui 不保存控件的**值**（滑块读数、复选框勾选、输入框内容都由调用方持
// 有缓冲）。忘持缓冲的症状是"拖一下弹回去"：控件显示的值每帧被外部旧值
// 覆盖。引擎把这部分状态收过来：按稳定 id 索引、跨帧存活，调用方的模式
// 变成"取 → 画 → 写回"，缓冲不再属于任何一个栈帧。
//
//   uint64_t id = GUi_MakeId("Opacity", entity_uuid);
//   float v = 0.5f; GUi_StateGetFloat(id, &v);
//   if (GUi_SliderFloat("Opacity", &v, 0, 1, "%.2f")) GUi_StateSetFloat(id, v);
//
// id 的稳定性来自 (label, seed)：同一个逻辑控件传同样的一对，跨帧同一个
// id。seed 取面板 / 实体 / 资产的身份（UUID、句柄、路径哈希）；全局控件
// 用 0。哈希冲突不处置（64 位在千级控件上概率可忽略，真冲突肉眼可辨）。
// ---------------------------------------------------------------------------

// label 与 seed 合成 64 位稳定 id。恒非 0（0 是状态存储的空槽标记）。
GRYCE_UI_API uint64_t GUi_MakeId(const char* label, uint64_t seed);

// 帧推进：渲染层每帧自动调；自持帧循环的宿主手动调。帧号同时是状态老化
// 的时间基准（超过 ~900 帧未被引用的条目会被回收，可见控件每帧都在引用）。
GRYCE_UI_API void  GUi_StateBeginFrame(uint32_t frame);

// 读：返回 1 = 该 id 有状态（*out 已写入）；0 = 没有（*out 不被触碰）。
// 写：不存在则新建（标量清零、文本清空）。
GRYCE_UI_API int   GUi_StateGetFloat(uint64_t id, float* out);
GRYCE_UI_API int   GUi_StateGetInt(uint64_t id, int* out);
GRYCE_UI_API int   GUi_StateGetBool(uint64_t id, int* out);
GRYCE_UI_API int   GUi_StateGetVec4(uint64_t id, float out[4]);
// 文本：至多拷贝 cap-1 字符。返回实际长度（不含结尾 '\0'），0 = 没有状态。
GRYCE_UI_API int   GUi_StateGetText(uint64_t id, char* out, int cap);
GRYCE_UI_API void  GUi_StateSetFloat(uint64_t id, float v);
GRYCE_UI_API void  GUi_StateSetInt(uint64_t id, int v);
GRYCE_UI_API void  GUi_StateSetBool(uint64_t id, int v);
GRYCE_UI_API void  GUi_StateSetVec4(uint64_t id, const float v[4]);
GRYCE_UI_API void  GUi_StateSetText(uint64_t id, const char* s);

// 当前持有状态的控件数（调试 / 测试用：验证状态没有无限膨胀）。
GRYCE_UI_API int   GUi_StateCount(void);
GRYCE_UI_API void  GUi_StateClear(void);

// ---------------------------------------------------------------------------
// 布局系统（确定性矩形计算）
//
// 服务"需要精确矩形"的场景：视口分区、gizmo 原点、属性行对齐。控件照旧
// 用上面的 GUi_* 画，画在哪由布局栈算。纯函数：相同调用序列 → 相同矩形，
// 与窗口 / ImGui / GPU 无关，因此"任意窗口尺寸下视口拿到确定矩形"可直接
// 单测（见 docs/zh/ui-api.md）。
//
// 注意不要对同一区域混用 ImGui 自动排版（GUi_BeginWindow 自带）与本布局栈。
// ---------------------------------------------------------------------------

typedef struct GRUiRect {
    float x, y, w, h;   // 绝对坐标，左上角原点，逻辑像素
} GRUiRect;

// 重置到视口根区域（一帧开头调一次）。
GRYCE_UI_API void  GUi_LayoutReset(const GRUiRect* viewport);
GRYCE_UI_API void  GUi_LayoutBeginPanel(const GRUiRect* area, float pad);
GRYCE_UI_API void  GUi_LayoutEndPanel(void);
GRYCE_UI_API int   GUi_LayoutDepth(void);
// 当前面板内容区（已扣内边距）与剩余高度。
GRYCE_UI_API void  GUi_LayoutContentRect(GRUiRect* out);
GRYCE_UI_API float GUi_LayoutAvailHeight(void);

// 行布局。begin_row 会先结束尚未 end_row 的行（防忘）；end_row 把光标
// 下移 行高 + 间距。cell 的 w <= 0 = 填满本行剩余宽度，h <= 0 = 用行高。
GRYCE_UI_API void  GUi_LayoutBeginRow(float height);
GRYCE_UI_API void  GUi_LayoutEndRow(void);
GRYCE_UI_API int   GUi_LayoutCell(float w, float h, GRUiRect* out);   // 1 = 成功
GRYCE_UI_API void  GUi_LayoutSpace(float px);
// 水平缩进：作用于下一次 begin_row 的行首（树形层级每层一级）。
GRYCE_UI_API void  GUi_LayoutIndent(float px);

// ---------------------------------------------------------------------------
// 组件树布局（保留模式）
//
// 与上面的布局栈分工：布局栈是即时模式——矩形按调用序列一帧一算，服务视口
// 分区、gizmo 原点这类"每帧 imperative 算矩形"的场景；组件树是保留模式——
// 结构作为数据跨帧存活，resolve() 一次算完整棵树，服务"面板定义"这类
// "结构是事实、不该每帧重演"的场景。两者可共存：组件树算出的矩形可以直接
// 喂给 GUi_* 画。
//
// 定位规则只有一条：**父级是不是布局组件**。
//   * 父级是布局组件（Column / Row / Grid）-> 子节点矩形由布局算出并写回；
//   * 父级不是布局组件（Leaf，或无父级）-> 子节点保留调用方写的 pos，
//     即"直接写 pos"。
// 布局只摆位、不决定容器自身大小；容器内容区 = rect 向内缩 padding。
//
// 全部函数都是纯数据操作（不画任何东西），回调内外都可调。
// ---------------------------------------------------------------------------

enum {
    GUI_COMP_KIND_LEAF   = 0,
    GUI_COMP_KIND_COLUMN = 1,   // 垂直栈
    GUI_COMP_KIND_ROW    = 2,   // 水平栈
    GUI_COMP_KIND_GRID   = 3,   // 网格（等宽列，行高取行内请求高最大值）
};

enum {
    GUI_COMP_ALIGN_START   = 0,
    GUI_COMP_ALIGN_CENTER  = 1,
    GUI_COMP_ALIGN_END     = 2,
    GUI_COMP_ALIGN_STRETCH = 3,
};

typedef struct GUiCompSpec {
    uint64_t parent;        // 0 = 无父级；否则必须是已存在的组件 id
    int      kind;          // GUI_COMP_KIND_*
    float    x, y, w, h;    // 自身几何（绝对坐标，左上角原点）
    float    margin[4];     // 上 右 下 左（作为子节点时被父布局读取）
    float    flex;          // 主轴弹性权重；>0 参与剩余空间分配，0 = 用 w/h
    int      align;         // GUI_COMP_ALIGN_*（交叉轴）
    float    gap;           // 布局组件：子节点间距
    float    padding[4];    // 布局组件：内容区内边距（上 右 下 左）
    int      cols;          // Grid 列数
} GUiCompSpec;

// 添加。返回新 id（恒非 0）；0 = 失败（表满 / parent 不存在 / 参数越界）。
GRYCE_UI_API uint64_t GUi_CompAdd(const GUiCompSpec* spec);
// 整体覆盖（parent 可改）。新父级是自己或自己的后代时拒绝。1 = 成功。
GRYCE_UI_API int      GUi_CompSet(uint64_t id, const GUiCompSpec* spec);
// 移除组件及其整棵子树。1 = 移除了什么，0 = 没有这个 id。
GRYCE_UI_API int      GUi_CompRemove(uint64_t id);
GRYCE_UI_API void     GUi_CompClear(void);
// 布局解析。返回解析到的组件数（幂等：同一棵树解析多次结果相同）。
GRYCE_UI_API int      GUi_CompResolve(void);
// 解析后的矩形。1 = 有该组件且 *out 已写入。
GRYCE_UI_API int      GUi_CompRect(uint64_t id, GRUiRect* out);
// 读回当前值（含解析后的 rect）。1 = 有该组件。
GRYCE_UI_API int      GUi_CompGet(uint64_t id, GUiCompSpec* out);
// 种类（GUI_COMP_KIND_*）；-1 = 没有该组件。
GRYCE_UI_API int      GUi_CompKind(uint64_t id);
// 父级 id；0 = 无父级或没有该组件。
GRYCE_UI_API uint64_t GUi_CompParent(uint64_t id);
// 子节点枚举（插入顺序）。-1 = 没有该组件。
GRYCE_UI_API int      GUi_CompChildCount(uint64_t id);
GRYCE_UI_API uint64_t GUi_CompChildAt(uint64_t id, int index);
GRYCE_UI_API int      GUi_CompCount(void);

// ---------------------------------------------------------------------------
// 主题（样式 token）
//
// 颜色按语义命名、度量按用途命名；调用方只引用 token，不引用色值。
// apply() 把整张表一次性推成 ImGui Style（帧外调；无 ImGui 构建为 no-op）。
// 语义文本色（GUI_TEXT_DIM / OK / WARN / ERROR / ACCENT）同样取自本表。
//
// 预设：每套预设只定义 17 个锚色，其余颜色 token 由引擎按固定规则派生
// （hover = 提亮、separator = border ...），所以换预设不会漏掉半个界面。
// token 编号与 ui_theme.h 逐一对齐（ui_api.cpp 里有静态断言钉死），
// 新增 token 只能加在枚举末尾。
// ---------------------------------------------------------------------------

enum {
    GUI_THEME_PRESET_DARK  = 0,
    GUI_THEME_PRESET_LIGHT = 1,
    GUI_THEME_PRESET_MIDNIGHT = 2,
    GUI_THEME_PRESET_NORD  = 3,
    GUI_THEME_PRESET_HIGH_CONTRAST = 4,
    GUI_THEME_PRESET_COUNT = 5,
};

enum {
    GUI_THEME_COLOR_WINDOW_BG = 0,
    GUI_THEME_COLOR_CHILD_BG,
    GUI_THEME_COLOR_BORDER,
    GUI_THEME_COLOR_TEXT,
    GUI_THEME_COLOR_TEXT_DIM,
    GUI_THEME_COLOR_ACCENT,
    GUI_THEME_COLOR_ACCENT_HOVER,
    GUI_THEME_COLOR_ACCENT_ACTIVE,
    GUI_THEME_COLOR_BUTTON,
    GUI_THEME_COLOR_BUTTON_HOVER,
    GUI_THEME_COLOR_BUTTON_ACTIVE,
    GUI_THEME_COLOR_HEADER,
    GUI_THEME_COLOR_HEADER_HOVER,
    GUI_THEME_COLOR_SELECTION,
    GUI_THEME_COLOR_OK,
    GUI_THEME_COLOR_WARN,
    GUI_THEME_COLOR_ERROR,
    // ---- 美化扩充（由锚色派生，预设不直接给值） ----
    GUI_THEME_COLOR_POPUP_BG,
    GUI_THEME_COLOR_TITLE_BG,
    GUI_THEME_COLOR_TITLE_BG_ACTIVE,
    GUI_THEME_COLOR_TITLE_BG_COLLAPSED,
    GUI_THEME_COLOR_MENUBAR_BG,
    GUI_THEME_COLOR_SCROLLBAR_BG,
    GUI_THEME_COLOR_SCROLLBAR_GRAB,
    GUI_THEME_COLOR_SCROLLBAR_GRAB_HOVERED,
    GUI_THEME_COLOR_SCROLLBAR_GRAB_ACTIVE,
    GUI_THEME_COLOR_SEPARATOR,
    GUI_THEME_COLOR_SEPARATOR_HOVERED,
    GUI_THEME_COLOR_SEPARATOR_ACTIVE,
    GUI_THEME_COLOR_RESIZE_GRIP,
    GUI_THEME_COLOR_RESIZE_GRIP_HOVERED,
    GUI_THEME_COLOR_RESIZE_GRIP_ACTIVE,
    GUI_THEME_COLOR_CHECK_MARK,
    GUI_THEME_COLOR_SLIDER_GRAB,
    GUI_THEME_COLOR_SLIDER_GRAB_ACTIVE,
    GUI_THEME_COLOR_HEADER_ACTIVE,
    GUI_THEME_COLOR_PLOT_LINES,
    GUI_THEME_COLOR_PLOT_LINES_HOVERED,
    GUI_THEME_COLOR_PLOT_HISTOGRAM,
    GUI_THEME_COLOR_PLOT_HISTOGRAM_HOVERED,
    GUI_THEME_COLOR_TABLE_HEADER_BG,
    GUI_THEME_COLOR_TABLE_BORDER_STRONG,
    GUI_THEME_COLOR_TABLE_BORDER_LIGHT,
    GUI_THEME_COLOR_TABLE_ROW_BG,
    GUI_THEME_COLOR_TABLE_ROW_BG_ALT,
    GUI_THEME_COLOR_DRAG_DROP_TARGET,
    GUI_THEME_COLOR_NAV_HIGHLIGHT,
    GUI_THEME_COLOR_NAV_WINDOWING_HIGHLIGHT,
    GUI_THEME_COLOR_NAV_WINDOWING_DIM_BG,
    GUI_THEME_COLOR_MODAL_WINDOW_DIM_BG,
    GUI_THEME_COLOR_BORDER_SHADOW,
    GUI_THEME_COLOR_TAB,
    GUI_THEME_COLOR_TAB_HOVERED,
    GUI_THEME_COLOR_TAB_SELECTED,
    GUI_THEME_COLOR_TAB_SELECTED_OVERLINE,
    GUI_THEME_COLOR_TAB_DIMMED,
    GUI_THEME_COLOR_TAB_DIMMED_SELECTED,
    GUI_THEME_COLOR_CONTROL_BORDER,
    GUI_THEME_COLOR_COUNT,
};

enum {
    GUI_THEME_METRIC_PAD_X = 0,     // 控件水平内边距
    GUI_THEME_METRIC_PAD_Y,         // 控件垂直内边距
    GUI_THEME_METRIC_ROW_H,         // 标准行高
    GUI_THEME_METRIC_ITEM_SPACING,  // 行 / 单元格间距
    GUI_THEME_METRIC_INDENT,        // 树形每级缩进
    GUI_THEME_METRIC_BORDER_W,      // 边框宽度
    // ---- 美化扩充（圆角/滚动条） ----
    GUI_THEME_METRIC_WINDOW_ROUNDING,
    GUI_THEME_METRIC_CHILD_ROUNDING,
    GUI_THEME_METRIC_FRAME_ROUNDING,
    GUI_THEME_METRIC_GRAB_ROUNDING,
    GUI_THEME_METRIC_POPUP_ROUNDING,
    GUI_THEME_METRIC_TAB_ROUNDING,
    GUI_THEME_METRIC_SCROLLBAR_SIZE,
    GUI_THEME_METRIC_GRAB_MIN_SIZE,
    GUI_THEME_METRIC_COUNT,
};

GRYCE_UI_API void     GUi_ThemeSetPreset(int preset);
GRYCE_UI_API uint32_t GUi_ThemeColor(int token);        // 0xRRGGBBAA
GRYCE_UI_API void     GUi_ThemeSetColor(int token, uint32_t rgba);
GRYCE_UI_API float    GUi_ThemeMetric(int token);
GRYCE_UI_API void     GUi_ThemeApply(void);
// 预设数量与显示名（供组合框枚举）。返回名字长度；越界 preset 返回 0。
GRYCE_UI_API int      GUi_ThemePresetCount(void);
GRYCE_UI_API int      GUi_ThemePresetName(int preset, char* out_buf, int cap);

// JSON 主题（见 docs/zh/ui-api.md）：anchors/colors/metrics/animation 四块，
// 部分文件合法（缺项由派生规则与出厂默认补齐）。返回 1/0（0 = 非法/读不到）。
GRYCE_UI_API int      GUi_ThemeLoadFromJson(const char* json_text);
GRYCE_UI_API int      GUi_ThemeLoadFromFile(const char* path);

// ---------------------------------------------------------------------------
// 动画系统（两个块：纯函数层 + 运行时）
//
// 块一（GUi_AnimEase / GUi_AnimTween / GUi_AnimColorLerp）是纯函数：给定
// 时间就算出值，不持有跨帧状态。"改一个属性并让它平滑过渡"用它们就够了，
// 不需要引入运行时。
//
// 块二（GUi_Anim* 播放控制 + 通道读取）是运行时：片段 = 一组把值写进具名
// 通道的轨道；播放器按 dt 推进、按循环模式折叠时间、把评估结果写进通道。
// 宿主再决定通道值用在哪里（组件 rect / 窗口位置 / 任何数值属性）——动画
// 不认识组件，组件也不依赖动画。
//
// 时间源由调用方给（advance 的 dt），模型层不读时钟：单测里时间是精确的，
// 帧循环里推进多少完全由宿主决定（UI 低刷新率时传放大后的 dt 即可）。
// ---------------------------------------------------------------------------

enum {
    GUI_ANIM_EASE_LINEAR = 0,
    GUI_ANIM_EASE_IN_QUAD,
    GUI_ANIM_EASE_OUT_QUAD,
    GUI_ANIM_EASE_IN_OUT_QUAD,
    GUI_ANIM_EASE_IN_CUBIC,
    GUI_ANIM_EASE_OUT_CUBIC,
    GUI_ANIM_EASE_IN_OUT_CUBIC,
    GUI_ANIM_EASE_OUT_BACK,      // 回弹（区间中段会超出 1）
    GUI_ANIM_EASE_IN_OUT_BACK,
    GUI_ANIM_EASE_OUT_ELASTIC,   // 弹性（过冲并振荡）
    GUI_ANIM_EASE_OUT_BOUNCE,    // 落地反弹
    GUI_ANIM_EASE_COUNT,
};

enum {
    GUI_ANIM_LOOP_ONCE = 0,      // 播到结尾停下
    GUI_ANIM_LOOP_LOOP = 1,      // 到头回开头
    GUI_ANIM_LOOP_PINGPONG = 2,  // 到头反向
};

typedef struct GUiAnimKey {
    float t;      // 时刻（秒，相对片段起点）
    float v;      // 值
    int   ease;   // 本帧到下一帧的缓动（GUI_ANIM_EASE_*）
} GUiAnimKey;

typedef struct GUiAnimTrack {
    char   channel[24];   // 目标通道名（超长截断）
    float  delay;         // 轨道延迟（错峰）；延迟期间保持初始值
    int    is_color;      // 0 = 标量轨道（用 keys）；1 = 颜色轨道（用 color_*）
    int    ease;          // 颜色轨道的缓动
    float  duration;      // 颜色轨道时长
    float  color_from[4];
    float  color_to[4];
    int       key_count;
    GUiAnimKey keys[16];
} GUiAnimTrack;

typedef struct GUiAnimClip {
    char  name[32];
    float duration;       // >0 = 显式时长；0 = 由轨道推出
    int   loop;           // GUI_ANIM_LOOP_*
    int   track_count;
    GUiAnimTrack tracks[8];
} GUiAnimClip;

// ---- 块一：纯函数（回调内外都可调，无任何状态） ----

GRYCE_UI_API float GUi_AnimEase(int ease, float t);
// 补间：from -> to，duration 秒、delay 秒延迟、ease 缓动；t 是补间开始后
// 经过的秒数。结果写入 *out。
GRYCE_UI_API void  GUi_AnimTween(float from, float to, float duration, float delay,
                                 int ease, float t, float* out);
GRYCE_UI_API void  GUi_AnimColorLerp(const float a[4], const float b[4], float t,
                                     float out[4]);
// 缓动显示名（越界写空串）。返回名字长度。
GRYCE_UI_API int   GUi_AnimEaseName(int ease, char* out_buf, int cap);

// ---- 块二：运行时 ----

// 片段实际时长（显式 duration 优先，否则由轨道推出）。
GRYCE_UI_API float GUi_AnimClipDuration(const GUiAnimClip* clip);
// 播放（内部拷贝片段）。restart=0 时保留当前时间（改循环模式/参数时用）。
GRYCE_UI_API void  GUi_AnimPlay(const GUiAnimClip* clip, int restart);
GRYCE_UI_API void  GUi_AnimStop(void);     // 停止；通道保留最后的值
GRYCE_UI_API void  GUi_AnimPause(void);
GRYCE_UI_API void  GUi_AnimResume(void);   // 播完之后 resume 会从头再来
GRYCE_UI_API void  GUi_AnimSetSpeed(float speed);
GRYCE_UI_API void  GUi_AnimSeek(float t);
// 按 dt 秒推进并评估全部轨道（未播放时 no-op）。帧循环里调，锁外。
GRYCE_UI_API void  GUi_AnimAdvance(float dt);

GRYCE_UI_API int   GUi_AnimIsPlaying(void);
GRYCE_UI_API int   GUi_AnimIsFinished(void);
GRYCE_UI_API float GUi_AnimTime(void);

// 通道枚举与读取。标量通道用 ChannelFloat，颜色通道用 ChannelColor；
// 类型不匹配或没有该通道时返回 0 / 0（不创建通道）。
GRYCE_UI_API int   GUi_AnimChannelCount(void);
GRYCE_UI_API int   GUi_AnimChannelName(int index, char* out_buf, int cap);
GRYCE_UI_API int   GUi_AnimChannelFloat(const char* name, float* out);
GRYCE_UI_API int   GUi_AnimChannelColor(const char* name, float out[4]);

// ---------------------------------------------------------------------------
// 输入门控与快捷键
//
// 引擎策略：派发任何快捷键之前先问 UI 要不要这个键盘。编辑器真实踩过
// "输入框里打 Wire 把工具切走"——各自判断就会各自漏，所以这是所有宿主的
// 唯一入口。快捷键表在注册期报冲突，不等用户按下去才发现两个功能抢一个键。
// ---------------------------------------------------------------------------

// UI 正吃着键盘时为 0（不要派发快捷键）。UI 没在跑时为 1。
GRYCE_UI_API int   GUi_ShouldDispatchShortcut(void);

// 注册 / 查询。mods 取 GUI_MOD_* 位或。add 返回 1 = 成功，0 = 冲突或表满；
// find 返回命令号，没有返回 -1。key 是宿主键码（GLFW_KEY_* 这类），本层
// 不认识语义，只做精确匹配。
GRYCE_UI_API int   GUi_ShortcutAdd(uint32_t key, uint32_t mods, int command);
GRYCE_UI_API int   GUi_ShortcutFind(uint32_t key, uint32_t mods);
GRYCE_UI_API void  GUi_ShortcutClear(void);

// ---------------------------------------------------------------------------
// 帧与状态
// ---------------------------------------------------------------------------

// 当前是否处于 UI 回调帧内。回调外调用任何 GUi_* 都不会崩，但也没有效果；
// 需要用"画了没有"做判断时用它，而不是靠返回值猜。
GRYCE_UI_API int   GUi_InFrame(void);

// 显示尺寸（逻辑像素）与缩放。
GRYCE_UI_API void  GUi_GetDisplaySize(float* out_w, float* out_h);

// ---------------------------------------------------------------------------
// 窗口 / 容器
// ---------------------------------------------------------------------------

GRYCE_UI_API int   GUi_BeginWindow(const char* name, float x, float y, float w, float h,
                                   int flags);
GRYCE_UI_API void  GUi_EndWindow(void);

// 子区域（面板）。w/h 传 0 表示"填满剩余空间"。
GRYCE_UI_API int   GUi_BeginChild(const char* id, float w, float h, int border);
GRYCE_UI_API void  GUi_EndChild(void);

// 菜单栏：必须在带 GUI_WIN_MENU_BAR 的窗口内调用。
GRYCE_UI_API int   GUi_BeginMenuBar(void);
GRYCE_UI_API void  GUi_EndMenuBar(void);
GRYCE_UI_API int   GUi_BeginMenu(const char* label);
GRYCE_UI_API void  GUi_EndMenu(void);
// 返回 1 表示本帧被点击。
GRYCE_UI_API int   GUi_MenuItem(const char* label, const char* shortcut, int enabled);

// ---------------------------------------------------------------------------
// 基础控件
// ---------------------------------------------------------------------------

GRYCE_UI_API void  GUi_Text(const char* text);
GRYCE_UI_API void  GUi_TextColored(int color, const char* text);
GRYCE_UI_API void  GUi_Separator(void);
GRYCE_UI_API void  GUi_Spacing(void);
GRYCE_UI_API void  GUi_SameLine(float spacing);
GRYCE_UI_API void  GUi_LabelText(const char* label, const char* text);

GRYCE_UI_API int   GUi_Button(const char* label);
GRYCE_UI_API int   GUi_ButtonSize(const char* label, float w, float h);
GRYCE_UI_API int   GUi_SmallButton(const char* label);
// 选中态可切换的行（层级树、资产列表都用它）。
GRYCE_UI_API int   GUi_Selectable(const char* label, int selected);
// 整行可点、且带右键菜单的条目在 ImGui 里要显式允许重叠，否则点击会被
// 前后的 Selectable 抢走。
GRYCE_UI_API void  GUi_SetItemAllowOverlap(void);

GRYCE_UI_API int   GUi_Checkbox(const char* label, int* inout_value);
// 返回 1 表示值被改过（拖动中每帧都可能为 1）。inout 就地更新。
GRYCE_UI_API int   GUi_SliderFloat(const char* label, float* inout_value,
                                   float min_value, float max_value, const char* fmt);
GRYCE_UI_API int   GUi_DragFloat(const char* label, float* inout_value, float speed);
GRYCE_UI_API int   GUi_DragFloat2(const char* label, float v[2], float speed);
GRYCE_UI_API int   GUi_DragFloat3(const char* label, float v[3], float speed);
GRYCE_UI_API int   GUi_DragFloat4(const char* label, float v[4], float speed);
// 颜色：带色块的四个数值框。比四个 DragFloat 好在能直接看到颜色，
// 而且不用担心把 0~1 拖成负数（内部会夹住）。
GRYCE_UI_API int   GUi_ColorEdit4(const char* label, float v[4]);
// 纯色块按钮：rgba 由调用方给（主题 token / 动画通道的值），点击返回 1。
// 调色板、动画颜色演示用它把"一个颜色值"直接显示出来。
GRYCE_UI_API int   GUi_ColorButton(const char* label, const float rgba[4]);
// 返回 1 表示内容被改过（拖动中每帧都可能为 1）。要"提交"语义（回车/失焦）
// 用 GUi_IsItemDeactivatedAfterEdit 配合判断。buf 就地更新。
GRYCE_UI_API int   GUi_InputText(const char* label, char* buf, int buf_size);
GRYCE_UI_API void  GUi_SetKeyboardFocusHere(void);
GRYCE_UI_API int   GUi_IsItemDeactivatedAfterEdit(void);
GRYCE_UI_API int   GUi_Combo(const char* label, int* current,
                             const char* const* items, int count);
GRYCE_UI_API void  GUi_ProgressBar(float fraction, float w, float h, const char* overlay);

// ---------------------------------------------------------------------------
// 树 / 折叠
// ---------------------------------------------------------------------------

// 返回 1 = 当前是展开的（需要配套 GUi_TreePop）。default_open 控制初始状态。
GRYCE_UI_API int   GUi_TreeNode(const char* label, int default_open);
GRYCE_UI_API void  GUi_TreePop(void);
// 不带缩进的折叠头（检视器的组件分组用它）。
GRYCE_UI_API int   GUi_CollapsingHeader(const char* label, int default_open);

// ---------------------------------------------------------------------------
// 图片（视口）
// ---------------------------------------------------------------------------

// tex 是纹理 id，**按当前 UI 轨取不同的那个**（GRender_GetUIBackend 可查）：
//   * ImGui 轨（默认）：GRender_GetImGuiTextureId 的返回值（ImTextureID）；
//   * 内置轨：GRender_GetUiTextureId 的返回值（打包的引擎纹理句柄）。
// 两者的 id 空间互不通用，混用在内置轨下表现为"图不显示"（句柄解出来是别的
// 槽位或直接无效）。回调内不能调 GRender_*，所以 id 要在回调外（tick）取好。
//
// 上下方向由引擎按后端自动处理（OpenGL 的渲染目标第一行是图像下边，需要翻 V；
// D3D12 / Vulkan 不翻），调用方不用也不该自己传 UV —— 传了就等于替引擎做了
// 一件它已经做过的事，GL 下会翻两次。
//
// w / h 必须都 > 0（与 ImGui 的 Image 一致，只收显式尺寸）。
//
// 与 ImGui 的 Image 的差别只在激活/导航：ImGui 那边图片没有 id（不进导航、不进
// ActiveId），内置轨给图片一个由纹理句柄派生的 id。悬停两条轨都报得出来
// （ImGui 的 IsItemHovered 是 LastItemData 的纯矩形判定，与 id 无关），所以
// GUi_Image 之后紧跟 GUi_LastItemRect / GUi_IsItemHovered 的拾取链两边一致
// （编辑器视口就是这么做的）。
GRYCE_UI_API void  GUi_Image(uint64_t tex, float w, float h);

// ---------------------------------------------------------------------------
// 弹出菜单
// ---------------------------------------------------------------------------

// 在"上一个条目"上按右键时打开 id 对应的 popup（右键菜单的常规写法）。
GRYCE_UI_API void  GUi_OpenPopupOnItemClick(const char* id, int mouse_button);
GRYCE_UI_API void  GUi_OpenPopup(const char* id);
GRYCE_UI_API int   GUi_BeginPopup(const char* id);
GRYCE_UI_API void  GUi_EndPopup(void);
// 由 popup 内的条目关掉当前 popup。没有它，点完菜单项弹窗会一直留着
// （ImGui 只在点到弹窗外面时才自动关）。
GRYCE_UI_API void  GUi_CloseCurrentPopup(void);

// 工具提示：画在鼠标右下方。内置轨带 0.35 秒悬停延时（宿主时钟为 0 时立即出，
// 便于单测与截图）；ImGui 轨是即时显示（延时由 ImGui 的 hover 计时管）。
GRYCE_UI_API void  GUi_SetTooltip(const char* text);

// 模态弹层：屏幕遮罩 + 居中面板。点面板外面**不关**，Esc 或
// GUi_CloseCurrentPopup 才关。宽高 <= 0 时用默认尺寸。
// 用法与 popup 一致：Open 一次，之后每帧 Begin/End。
GRYCE_UI_API void  GUi_OpenModal(const char* name, float w, float h);
GRYCE_UI_API int   GUi_BeginModal(const char* name, float w, float h);

// ---------------------------------------------------------------------------
// 拖放
//
// payload 只支持字符串。编辑器里要传的东西（实体句柄、资产路径、组件类型名）
// 本来就有文本形式，走文本换来两件事：跨 DLL 不依赖任何内存布局，
// 而且调试时能在 tooltip 里直接看到内容。
//
// 用法（源在条目之后立刻调用）：
//     if (GUi_BeginDragDropSource()) {
//         char id[24]; snprintf(id, sizeof(id), "%lld", (long long)handle);
//         GUi_SetDragDropPayloadString("GRYCE_ENTITY", id);
//         GUi_EndDragDropSource();
//     }
// 目标（同样是"上一个条目"）：
//     if (GUi_BeginDragDropTarget()) {
//         char buf[64];
//         if (GUi_AcceptDragDropPayloadString("GRYCE_ENTITY", buf, sizeof(buf))
//                 == GUI_DRAG_DELIVER) { ... }
//         GUi_EndDragDropTarget();
//     }
// ---------------------------------------------------------------------------

// 当前条目是否被按住并拖动到阈值之外（即"拖起来"了）。鼠标键固定为左键，
// 这是 ImGui 的约定，没有按其它键拖的开关。
GRYCE_UI_API int   GUi_BeginDragDropSource(void);
GRYCE_UI_API int   GUi_SetDragDropPayloadString(const char* type, const char* text);
GRYCE_UI_API void  GUi_EndDragDropSource(void);

GRYCE_UI_API int   GUi_BeginDragDropTarget(void);
// 返回 GUI_DRAG_*。只有 GUI_DRAG_DELIVER 时 out_buf 才被写入。
GRYCE_UI_API int   GUi_AcceptDragDropPayloadString(const char* type,
                                                   char* out_buf, int buf_size);
GRYCE_UI_API void  GUi_EndDragDropTarget(void);

// ---------------------------------------------------------------------------
// 布局 / 命中查询
// ---------------------------------------------------------------------------

GRYCE_UI_API void  GUi_SetCursorPos(float x, float y);
GRYCE_UI_API void  GUi_GetCursorPos(float* out_x, float* out_y);
GRYCE_UI_API void  GUi_GetContentAvail(float* out_w, float* out_h);
GRYCE_UI_API void  GUi_SetNextItemWidth(float w);
// 缩进 / 反缩进（树形层级用）。px <= 0 = 用主题的缩进度量。
GRYCE_UI_API void  GUi_Indent(float px);
GRYCE_UI_API void  GUi_Unindent(float px);
// 当前窗口的竖向滚动位置。内容超出窗口时内置轨会自动出现滚动条，宿主只在
// 需要"滚到某处"（选中项可见、Home/End 翻页）时才用这两个。
GRYCE_UI_API float GUi_GetWindowScrollY(void);
GRYCE_UI_API void  GUi_SetWindowScrollY(float v);
GRYCE_UI_API float GUi_GetTextLineHeight(void);
GRYCE_UI_API float GUi_GetFrameHeight(void);
// 文本宽度（像素）。检视器要用它算"标签占多宽"，再把控件宽度设成
// `-(标签宽 + 间距)` —— 直接给 -1 会让控件吃满整行，标签被挤到面板外。
GRYCE_UI_API float GUi_CalcTextWidth(const char* text);

// 上一个条目的屏幕矩形（绝对坐标，左上角原点）。视口图片的矩形就是靠它
// 拿到的：鼠标位置 → 视口坐标的换算必须基于**实际绘制到屏幕上的矩形**，
// 而不是请求的尺寸，否则缩放的窗口下点哪都偏。
GRYCE_UI_API void  GUi_LastItemRect(float* out_x, float* out_y,
                                    float* out_w, float* out_h);
GRYCE_UI_API int   GUi_IsItemHovered(void);
GRYCE_UI_API int   GUi_IsItemClicked(int mouse_button);
GRYCE_UI_API int   GUi_IsItemActive(void);
// 是否有任意控件正处于编辑态（拖拽中 / 输入框聚焦）。面板用它判断
// "一次连续编辑结束没有"，据此把整段拖拽合并成一步撤销。
GRYCE_UI_API int   GUi_IsAnyItemActive(void);

// 当前按住的修饰键（GUI_MOD_* 位或）。层级树用它区分"替换选择 / 加选 /
// 切换"，输入框与弹窗里也照常有效。
GRYCE_UI_API int   GUi_GetModifiers(void);

GRYCE_UI_API void  GUi_MousePos(float* out_x, float* out_y);
GRYCE_UI_API float GUi_MouseDeltaX(void);
GRYCE_UI_API float GUi_MouseDeltaY(void);
GRYCE_UI_API int   GUi_IsMouseDown(int button);
GRYCE_UI_API int   GUi_IsMouseClicked(int button);
GRYCE_UI_API int   GUi_IsMouseReleased(int button);

// 输入被 UI 吃掉时为 1。宿主据此决定"这次按键还给不给场景导航/快捷键"。
GRYCE_UI_API int   GUi_WantCaptureMouse(void);
GRYCE_UI_API int   GUi_WantCaptureKeyboard(void);

// ---------------------------------------------------------------------------
// 样式（少量，够标记状态色即可）
// ---------------------------------------------------------------------------

// kind 取 GUI_TEXT_*，push 之后的作用域到 GUi_PopTextStyle 为止。
GRYCE_UI_API void  GUi_PushTextStyle(int kind);
GRYCE_UI_API void  GUi_PopTextStyle(void);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // GRYCE_UI_API_H
