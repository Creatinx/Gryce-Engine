// ===========================================================================
// engine/api/ui_builtin_ops.h — 内置 UI 轨的 GUi_* 实现表
//
// 为什么是一张函数指针表而不是直接调 BuiltinUi：
// api/ui_api.cpp 同时编进 GryceRenderer（要接内置轨）和 GryceUiTests
// （模型层单测，**不链接**渲染器/平台）。直接 include ui_builtin_backend.h
// 会让测试目标多出一条它根本用不到的链接依赖；改成运行期填表则两边都能
// 编——测试里没人安装这张表，`builtin_ops()` 恒为空，所有 GUi_* 走 ImGui 分支
// （测试里也没有 ImGui，于是退化成原本的"帧外安全空操作"）。
//
// 表的签名只用 C 基本类型：不引入 Rect/Color/字符串类，避免把模型层头文件
// 泄漏到 ui_api.cpp 的编译面。
// ===========================================================================

#ifndef GRYCE_UI_BUILTIN_OPS_H
#define GRYCE_UI_BUILTIN_OPS_H

#include <cstdint>

namespace gryce_ui {

struct BuiltinUiOps {
    // 窗口 / 内容
    int  (*begin_window)(const char* name, float x, float y, float w, float h);
    void (*end_window)();
    void (*text)(const char* s);
    void (*text_colored)(uint32_t rgba, const char* s);
    void (*separator)();
    void (*spacing)(float px);
    void (*same_line)(float spacing);
    void (*label_text)(const char* label, const char* text);

    // 交互控件
    int  (*button)(const char* label);
    int  (*button_size)(const char* label, float w, float h);
    int  (*checkbox)(const char* label, int* inout_value);
    int  (*slider_float)(const char* label, float* inout_value, float vmin, float vmax);
    int  (*input_text)(const char* label, char* buf, int buf_size);
    int  (*color_button)(const char* label, const float rgba[4]);
    void (*progress_bar)(float fraction, const char* overlay);

    // 度量与光标流
    void  (*display_size)(float* out_w, float* out_h);
    float (*text_line_height)();
    float (*frame_height)();
    float (*calc_text_width)(const char* text);
    void  (*set_next_item_width)(float w);   // 内置轨暂无列宽概念：空操作
    void  (*get_content_avail)(float* out_w, float* out_h);
    void  (*set_cursor_pos)(float x, float y);

    // 刚提交 item 的查询
    void (*last_item_rect)(float* out_x, float* out_y, float* out_w, float* out_h);
    int  (*is_item_hovered)();
    int  (*is_item_active)();
    // 整棵树里有没有控件正被按住（连续编辑合并撤销的判据，见 ui-api.md）
    int  (*is_any_item_active)();

    // 原始鼠标状态（宿主在自己的输入处理里用）
    void (*mouse_pos)(float* out_x, float* out_y);
    int  (*is_mouse_down)(int button);
    int  (*is_mouse_clicked)(int button);
    int  (*is_mouse_released)(int button);
    int  (*want_capture_mouse)();
    int  (*want_capture_keyboard)();
};

// 已安装的实现表（null = 没有内置轨，只有 ImGui 轨）。ui_api.cpp 定义。
const BuiltinUiOps* builtin_ops();

// 安装/卸载（引擎初始化时由 ui_builtin_backend.cpp 调；传空 = 卸载）。
void install_builtin_ops(const BuiltinUiOps* ops);

// ---- 后端选择（进程级）----
// 0 = ImGui（默认），1 = 内置。ui_api.cpp 定义；渲染层在 GRender_Init 时读。
void set_ui_backend(int backend);
int  ui_backend();
bool builtin_ui();

} // namespace gryce_ui

#endif // GRYCE_UI_BUILTIN_OPS_H
