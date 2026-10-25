// ===========================================================================
// engine/api/ui_builtin_backend.cpp — 内置 UI 轨的运行时实现
//
// 这里只做"装配"：没有任何布局/交互逻辑（那些在 ui/ 里，有单测）。三件事：
//   1) 把平台输入翻成 UiInputFrame（鼠标、字符、退格）；
//   2) 给模型层注入主题与文本度量；
//   3) 把构建好的 drawlist 作为一条渲染命令推进命令缓冲；
//   4) 填 ui_builtin_ops.h 那张表，让 ui_api.cpp 的 GUi_* 能转到内置轨。
//
// 本文件编入 GryceRenderer（要用 IRenderer2D / RenderContext / 平台输入），
// 不进 GryceUiTests——测试在模型层，装配层的价值在于"接上了没有"，靠 smoke
// 实际截图验证。
// ===========================================================================

#include "api/ui_builtin_backend.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>

#include "api/input_api.h"
#include "render/render2d.h"
#include "ui/ui_render_bridge.h"
#include "ui/ui_theme.h"

namespace gryce_ui {

namespace {

// GLFW 键码（与 platform 的 keys[] 索引同一套，见 input.cpp 的 32..348）。
// 本帧"按下"的编辑键走这条通道（字符通道只送可打印码点）。
constexpr int kKeyBackspace = 259;
constexpr int kEditKeys[] = {
    263, 262, 268, 269, 261,   // Left Right Home End Delete
    259, 256, 258, 257, 265,   // Backspace Esc Tab Enter Up
    264, 65, 67, 86, 88,       // Down A C V X
};
constexpr int kEditKeyCount = static_cast<int>(sizeof(kEditKeys) / sizeof(kEditKeys[0]));

// GLFW 修饰键
constexpr int kKeyLeftShift = 340, kKeyRightShift = 344;
constexpr int kKeyLeftCtrl  = 341, kKeyRightCtrl  = 345;
constexpr int kKeyLeftAlt   = 342, kKeyRightAlt   = 346;
constexpr int kKeyLeftSuper = 343, kKeyRightSuper = 347;

// 单帧时钟（插入符闪烁用；宿主不需要自己的时钟）
float ui_clock() {
    static const auto t0 = std::chrono::steady_clock::now();
    const auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    return static_cast<float>(dt) * 0.001f;
}

// 文本度量：宽度走渲染器的真实字形推进量；渲染器不支持时退回估算。
// 两者共用 UiTextMetrics 接口，布局与绘制因此不会用两个宽度。
class RendererTextMetrics final : public UiTextMetrics {
public:
    RendererTextMetrics(gryce_engine::render::IRenderer2D* renderer, float scale)
        : renderer_(renderer), scale_((scale > 0.0f) ? scale : 1.0f) {}

    float width(const char* text, float font_size) const override {
        // 模型层用逻辑字号问宽度，渲染器按真实字号量，再换回逻辑单位——
        // 布局与绘制因此始终同一口径（HiDPI 下也不会错位）。真实字号必须与
        // 绘制侧一样取整（ui_text_physical_size），否则宽度按 19.5 算、字形按
        // 20 画，累计到一长串文字就能差出一个字。
        if (renderer_) {
            const float w = renderer_->measure_text(text, ui_text_physical_size(font_size, scale_));
            if (w > 0.0f) return w / scale_;
        }
        return ui_approx_text_width(text, font_size);
    }

    float line_height(float font_size) const override {
        // 行盒 = 1.25em：字形通常 0.7~0.8em，余量留给上下伸部与相邻行间隔。
        return font_size * 1.25f;
    }

private:
    gryce_engine::render::IRenderer2D* renderer_;
    float scale_ = 1.0f;
};

// 本帧输入快照。宿主没开窗口时所有查询返回 0/false，得到一份全零输入——
// 控件因此处于"没被碰过"的状态：画得出来，但不响应。
UiInputFrame collect_input() {
    UiInputFrame in;

    float mx = 0.0f;
    float my = 0.0f;
    GInput_GetMousePosition(&mx, &my);
    in.mouse_x = mx;
    in.mouse_y = my;

    for (int b = 0; b < 3; ++b) {
        in.mouse_down[b]     = GInput_IsMouseButtonHeld(b);
        in.mouse_clicked[b]  = GInput_IsMouseButtonPressed(b);
        in.mouse_released[b] = GInput_IsMouseButtonReleased(b);
    }

    in.char_count = GInput_PullChars(in.chars, UiInputFrame::kMaxChars);
    in.backspace  = GInput_IsKeyPressed(kKeyBackspace);

    // 编辑键（方向键/Home/End/Delete/Esc/Ctrl+字母）：只收"本帧按下"的边沿，
    // 按住不放不该让插入点连续跑（连发交给系统重复即可）。
    for (int i = 0; i < kEditKeyCount; ++i) {
        if (GInput_IsKeyPressed(kEditKeys[i])) {
            in.keys[in.key_count++] = kEditKeys[i];
        }
    }

    // 修饰键
    if (GInput_IsKeyHeld(kKeyLeftShift) || GInput_IsKeyHeld(kKeyRightShift)) {
        in.mods |= UiInputFrame::ModShift;
    }
    if (GInput_IsKeyHeld(kKeyLeftCtrl) || GInput_IsKeyHeld(kKeyRightCtrl)) {
        in.mods |= UiInputFrame::ModCtrl;
    }
    if (GInput_IsKeyHeld(kKeyLeftAlt) || GInput_IsKeyHeld(kKeyRightAlt)) {
        in.mods |= UiInputFrame::ModAlt;
    }
    if (GInput_IsKeyHeld(kKeyLeftSuper) || GInput_IsKeyHeld(kKeyRightSuper)) {
        in.mods |= UiInputFrame::ModSuper;
    }

    // 滚轮（取走即清零）
    GInput_GetWheel(nullptr, &in.wheel);

    // 输入法预编辑串
    GInput_GetPreedit(in.preedit, static_cast<int>(sizeof(in.preedit)));

    // 时钟：只给插入符闪烁用
    in.time = ui_clock();
    return in;
}

BuiltinRuntime& rt() { return BuiltinRuntime::instance(); }

// ---------------------------------------------------------------------------
// 实现表：把 GUi_* 的 C 签名翻到模型层
// ---------------------------------------------------------------------------
const BuiltinUiOps kOps = {
    // ---- 窗口 / 内容 ----
    [](const char* name, float x, float y, float w, float h, uint32_t flags) {
        return rt().ui().begin_window(name, Rect{x, y, w, h}, flags);
    },
    []() { rt().ui().end_window(); },
    [](const char* s) { rt().ui().text(s); },
    [](uint32_t rgba, const char* s) { rt().ui().text_colored(rgba, s); },
    []() { rt().ui().separator(); },
    [](float px) { rt().ui().spacing(px); },
    [](float spacing) { rt().ui().same_line(spacing); },
    [](const char* label, const char* text) { rt().ui().label_text(label, text); },
    [](float px) { rt().ui().indent(px); },
    [](float px) { rt().ui().unindent(px); },
    []() { return rt().ui().window_scroll_y(); },
    [](float v) { rt().ui().set_window_scroll_y(v); },

    // ---- 交互控件 ----
    [](const char* label) { return rt().ui().button(label); },
    [](const char* label, float w, float h) { return rt().ui().button_size(label, w, h); },
    [](const char* label, int* v) { return rt().ui().checkbox(label, v); },
    [](const char* label, float* v, float lo, float hi) {
        return rt().ui().slider_float(label, v, lo, hi);
    },
    [](const char* label, float* v, float speed, float lo, float hi) {
        return rt().ui().drag_float(label, v, speed, lo, hi);
    },
    [](const char* label, float* v, int count, float speed) {
        return rt().ui().drag_float_n(label, v, count, speed);
    },
    [](const char* label, int selected) {
        return rt().ui().selectable(label, selected);
    },
    [](uint64_t tex, float w, float h) { rt().ui().image(tex, w, h); },
    [](const char* label, char* buf, int cap) { return rt().ui().input_text(label, buf, cap); },
    [](const char* label, const float rgba[4]) { return rt().ui().color_button(label, rgba); },
    [](float fraction, const char* overlay) { rt().ui().progress_bar(fraction, overlay); },

    // ---- 结构 ----
    [](const char* label, int def) { return rt().ui().tree_node(label, def); },
    []() { rt().ui().tree_pop(); },
    [](const char* label, int def) { return rt().ui().collapsing_header(label, def); },
    []() { rt().ui().begin_menu_bar(); },
    []() { rt().ui().end_menu_bar(); },
    [](const char* label) { return rt().ui().begin_menu(label); },
    []() { rt().ui().end_menu(); },
    [](const char* label, const char* sc, int en) { return rt().ui().menu_item(label, sc, en); },
    [](const char* id, float w, float h, int border) { return rt().ui().begin_child(id, w, h, border); },
    []() { rt().ui().end_child(); },

    // ---- 弹层 ----
    [](const char* id, float x, float y, float w, float h) {
        rt().ui().open_popup(id, x, y, w, h);
    },
    [](const char* id, float w) { rt().ui().open_popup_on_item(id, w); },
    [](const char* id) { return rt().ui().begin_popup(id); },
    []() { rt().ui().end_popup(); },
    []() { rt().ui().close_current_popup(); },
    [](const char* id) { return rt().ui().is_popup_open(id); },
    [](const char* name, float w, float h) { rt().ui().open_modal(name, w, h); },
    [](const char* name, float w, float h) { return rt().ui().begin_modal(name, w, h); },
    [](const char* text) { rt().ui().set_tooltip(text); },

    // ---- 拖放 ----
    []() { return rt().ui().begin_drag_drop_source(); },
    [](const char* type, const char* text) { rt().ui().set_drag_drop_payload_string(type, text); },
    []() { rt().ui().end_drag_drop_source(); },
    []() { return rt().ui().begin_drag_drop_target(); },
    [](const char* type, char* out, int cap) {
        return rt().ui().accept_drag_drop_payload_string(type, out, cap);
    },
    []() { rt().ui().end_drag_drop_target(); },

    // ---- 度量与光标流 ----
    [](float* w, float* h) {
        if (w) *w = rt().display_width();
        if (h) *h = rt().display_height();
    },
    []() { return rt().line_height(); },
    []() { return rt().frame_height(); },
    [](const char* text) { return rt().text_width(text); },
    [](float w) { (void)w; },   // 内置轨没有列宽概念（控件总是撑满内容区）
    [](float* w, float* h) {
        // 内容区可用尺寸 = 当前窗口内缩一个内边距。无窗口时全 0。
        const Rect r = rt().ui().current_window_rect();
        const float pad_x = theme().metrics[THEME_METRIC_PAD_X];
        const float pad_y = theme().metrics[THEME_METRIC_PAD_Y];
        if (w) *w = (r.w > pad_x * 2.0f) ? (r.w - pad_x * 2.0f) : 0.0f;
        if (h) *h = (r.h > pad_y * 2.0f) ? (r.h - pad_y * 2.0f) : 0.0f;
    },
    [](float x, float y) { (void)x; (void)y; },   // 光标流由控件推进，宿主不能随意跳

    // ---- 刚提交 item 的查询 ----
    [](float* x, float* y, float* w, float* h) {
        const Rect r = rt().ui().last_item_rect();
        if (x) *x = r.x;
        if (y) *y = r.y;
        if (w) *w = r.w;
        if (h) *h = r.h;
    },
    []() { return rt().ui().is_item_hovered() ? 1 : 0; },
    []() { return rt().ui().is_item_active() ? 1 : 0; },
    [](int button) { return rt().ui().is_item_clicked(button) ? 1 : 0; },
    []() { return rt().ui().tree().active_id() != 0 ? 1 : 0; },

    // ---- 原始鼠标 / 键盘状态 ----
    [](float* x, float* y) {
        float mx = 0.0f;
        float my = 0.0f;
        GInput_GetMousePosition(&mx, &my);
        if (x) *x = mx;
        if (y) *y = my;
    },
    [](int button) { return GInput_IsMouseButtonHeld(button) ? 1 : 0; },
    [](int button) { return GInput_IsMouseButtonPressed(button) ? 1 : 0; },
    [](int button) { return GInput_IsMouseButtonReleased(button) ? 1 : 0; },
    []() {
        // 位：1 = Shift，2 = Ctrl，4 = Alt（与 GUI_MOD_* 对齐）
        const UiInputFrame& in = rt().ui().tree().input();
        int m = 0;
        if (in.mods & UiInputFrame::ModShift) m |= 1;
        if (in.mods & UiInputFrame::ModCtrl)  m |= 2;
        if (in.mods & UiInputFrame::ModAlt)   m |= 4;
        return m;
    },
    // WantCaptureMouse：鼠标落在任何一个面板/弹层里就不该交给视口。
    // WantCaptureKeyboard：输入框拿到焦点时不派发快捷键。
    []() { return rt().ui().wants_mouse() ? 1 : 0; },
    []() { return rt().ui().wants_keyboard() ? 1 : 0; },
};

} // namespace

void install_builtin_ui() {
    install_builtin_ops(&kOps);
}

// ---------------------------------------------------------------------------
// BuiltinRuntime
// ---------------------------------------------------------------------------

BuiltinRuntime& BuiltinRuntime::instance() {
    static BuiltinRuntime instance_;
    return instance_;
}

void BuiltinRuntime::set_frame_context(int width, int height,
                                       gryce_engine::render::IRenderer2D* renderer) {
    width_    = width  > 0 ? width  : 0;
    height_   = height > 0 ? height : 0;
    renderer_ = renderer;
}

void BuiltinRuntime::set_throttle(int interval, uint64_t frame) {
    interval_    = interval > 1 ? interval : 1;
    frame_       = frame;
    rebuild_due_ = !has_frame_ || interval_ <= 1 ||
                   (frame_ % static_cast<uint64_t>(interval_)) == 1;
}

bool BuiltinRuntime::begin_build() {
    if (!rebuild_due_) return false;
    if (width_ <= 0 || height_ <= 0) return false;

    metrics_ = std::make_unique<RendererTextMetrics>(renderer_, scale_);
    ui_.set_scale(scale_);
    // 弹层/遮罩要夹在屏幕内：模型层要逻辑尺寸
    ui_.set_display_size(static_cast<float>(width_) / scale_,
                         static_cast<float>(height_) / scale_);
    ui_.begin_frame(collect_input(), theme(), *metrics_);
    building_ = true;
    return true;
}

void BuiltinRuntime::end_build() {
    if (!building_) return;
    building_ = false;

    ui_.end_frame();   // 收帧：回收本帧未提交的控件、释放拖拽捕获
    list_.reset();
    ui_.draw(list_);

    has_frame_   = true;
    rebuild_due_ = false;
    ++build_count_;
    last_draws_  = list_.command_count();
}

void BuiltinRuntime::draw_now(gryce_engine::render::IRenderer2D* renderer) {
    if (!renderer || !has_frame_ || width_ <= 0 || height_ <= 0) return;

    // 主线程同步走一遍：覆盖层 pass 只记录顶点并把它们压成命令（与
    // render_2d_overlay 里画 FPS 文字是同一条路径），真正的 GL 调用仍然在
    // 渲染线程按命令序执行，因此 UI 命令自然排在场景命令之后、盖在画面上。
    renderer->begin_overlay_pass(width_, height_);
    {
        // 适配器与桥是无状态的短命对象：一次 pass 内建、用完即弃，
        // 没有任何 UI 对象跨帧持有渲染器指针。
        std::unique_ptr<UiRenderer> target = make_renderer2d_ui_renderer(renderer, scale_);
        UiRenderBridge bridge;
        bridge.render(list_, *target);
        last_draws_ = bridge.draw_commands();
    }
    renderer->end_overlay_pass();
}

float BuiltinRuntime::text_width(const char* text) const {
    const RendererTextMetrics metrics(renderer_, scale_);
    return metrics.width(text, kWidgetFontSize);
}

float BuiltinRuntime::line_height() const {
    return kWidgetFontSize * 1.25f;
}

float BuiltinRuntime::frame_height() const {
    const float pad_y = theme().metrics[THEME_METRIC_PAD_Y];
    return line_height() + pad_y * 2.0f;
}

void BuiltinRuntime::reset() {
    ui_.tree().clear();
    metrics_.reset();
    list_.reset();
    has_frame_   = false;
    building_    = false;
    rebuild_due_ = true;
    build_count_ = 0;
    last_draws_  = 0;
    width_       = 0;
    height_      = 0;
    renderer_    = nullptr;
    interval_    = 1;
    frame_       = 0;
}

} // namespace gryce_ui
