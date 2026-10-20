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
// 布局约定：本接口不提供 dock。编辑器用"一整块主窗口 + 固定分区"的布局，
// 理由见 editor/README.md：docking 会让布局随 imgui.ini 漂移，截图比对和
// 集成测试就不可复现了。
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

// tex 是 GRender_GetImGuiTextureId 的返回值。回调内不能调 GRender_*，
// 所以这个 ID 要在回调外（tick）取好再传进来。
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
