#pragma once

// GryceEngineUtils/ui/ui.h —— UI 模块主入口
// 包含所有 Widget 与 UIManager。

#include "GryceEngineUtils/ui/signal.h"
#include "GryceEngineUtils/ui/widget.h"
#include "GryceEngineUtils/ui/stylesheet.h"
#include "GryceEngineUtils/ui/animation.h"
#include "GryceEngineUtils/ui/label.h"
#include "GryceEngineUtils/ui/button.h"
#include "GryceEngineUtils/ui/panel.h"
#include "GryceEngineUtils/ui/image.h"
#include "GryceEngineUtils/ui/text_input.h"
#include "GryceEngineUtils/ui/slider.h"
#include "GryceEngineUtils/ui/checkbox.h"
#include "GryceEngineUtils/ui/progress_bar.h"
#include "GryceEngineUtils/ui/scroll_view.h"
#include "GryceEngineUtils/ui/divider.h"
#include "GryceEngineUtils/ui/toolbar.h"
#include "GryceEngineUtils/ui/menubar.h"
#include "GryceEngineUtils/ui/combobox.h"
#include "GryceEngineUtils/ui/radiobutton.h"
#include "GryceEngineUtils/ui/spinbox.h"
#include "GryceEngineUtils/ui/factory.h"
#include "ui/ui_renderer.h"
#include "ui/script_vm.h"
#include "ui/engine_bridge.h"

namespace GryceEngineUtils::ui {

class UIManager {
public:
    static UIManager* create(Renderer* renderer);
    void destroy();

    // 当前 UIManager（Animation::create 自动注册动画用）
    static UIManager* current() { return current_; }

    // 根节点
    void set_root(Widget* root);
    Widget* root() const { return root_; }

    // 每帧调用
    void update(float dt);
    void render(); // 在 Renderer::end_frame() 前调用；Renderer 已 set_ui_manager 时自动调用

    // 输入事件转发（Renderer 自动转发；也可手动调用）
    void on_mouse_move(float x, float y);
    void on_mouse_button(int button, bool down);
    void on_keyboard(int key, bool down);
    void on_text_input(const char* utf8);

    // 命中查询：返回 (x,y) 处最深的可见控件（模态优先）
    Widget* hit_test_top(float x, float y);

    // 样式表
    void set_global_style(StyleSheet* style);
    StyleSheet* global_style() const { return global_style_; }

    // 焦点管理
    Widget* focused_widget() const { return focused_; }
    void set_focus(Widget* widget);

    // 模态
    void push_modal(Widget* modal);
    void pop_modal();

    Renderer* renderer() const { return renderer_; }

    // 动画管理（Animation::create 自动调用）
    void add_animation(Animation* anim);
    void remove_animation(Animation* anim);

private:
    explicit UIManager(Renderer* renderer);
    ~UIManager();
    UIManager(const UIManager&) = delete;
    UIManager& operator=(const UIManager&) = delete;

    void draw_widget(Widget* widget);
    void update_hover(float x, float y);

    static UIManager* current_;

    Renderer* renderer_ = nullptr;
    gryce_engine::render::IRenderer2D* renderer2d_ = nullptr;
    UIRenderer* ui_renderer_ = nullptr;
    ScriptVM* script_vm_ = nullptr;
    Widget* root_ = nullptr;
    StyleSheet* global_style_ = nullptr;
    Widget* focused_ = nullptr;
    std::vector<Widget*> modals_;
    std::vector<Animation*> animations_;

    Widget* hovered_ = nullptr;
    Widget* pressed_widget_ = nullptr;
    int pressed_button_ = -1;
    float mouse_x_ = 0.0f;
    float mouse_y_ = 0.0f;
};

} // namespace GryceEngineUtils::ui
