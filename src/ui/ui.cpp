#include "GryceEngineUtils/ui/ui.h"

#include <algorithm>
#include <cstring>

#include "render/render2d.h"
#include "utils/glog/glog_lib.h"

namespace GryceEngineUtils::ui {

UIManager* UIManager::current_ = nullptr;

UIManager* UIManager::create(Renderer* renderer) {
    return new UIManager(renderer);
}

void UIManager::destroy() {
    delete this;
}

UIManager::UIManager(Renderer* renderer)
    : renderer_(renderer)
    , renderer2d_(renderer ? renderer->renderer2d() : nullptr)
    , ui_renderer_(nullptr)
    , script_vm_(nullptr) {
    current_ = this;

    // 初始化 UIRenderer
    // 注意：不能通过 pause_render_thread()/resume_render_thread() 临时把 GL context
    // 切回主线程来编译 UI 着色器——那会重建 command buffer + 渲染线程，导致整帧黑屏。
    // 保持直接在此 init：若当前无主线程 context，编译失败即回退 2D 渲染器（HUD 照常绘制）。
    if (renderer_ && renderer_->context()) {
        ui_renderer_ = new UIRenderer();
        if (!ui_renderer_->init(renderer_->context())) {
            GLOG_WARN("UIManager: failed to init UIRenderer, falling back to 2D renderer");
            delete ui_renderer_;
            ui_renderer_ = nullptr;
        } else {
            GLOG_INFO("UIManager: UIRenderer initialized");
        }
    }

    // 初始化 ScriptVM（QuickJS 脚本引擎）
    script_vm_ = new ScriptVM();
    if (!script_vm_->init()) {
        GLOG_WARN("UIManager: failed to init ScriptVM, UI scripting disabled");
        delete script_vm_;
        script_vm_ = nullptr;
    } else {
        GLOG_INFO("UIManager: ScriptVM initialized");
        // 注册 engine.* SDK 函数
        EngineBridge::init(script_vm_, this);
    }
}

UIManager::~UIManager() {
    if (current_ == this) current_ = nullptr;
    delete ui_renderer_;
    ui_renderer_ = nullptr;
    delete script_vm_;
    script_vm_ = nullptr;
    for (auto* anim : animations_) delete anim;
    animations_.clear();
    delete global_style_;
    global_style_ = nullptr;
    for (auto* modal : modals_) delete modal;
    modals_.clear();
    delete root_;
    root_ = nullptr;
}

void UIManager::set_root(Widget* root) {
    root_ = root;
    // 根控件禁止鼠标事件，避免全屏背景因鼠标移动切换 hover 状态导致背景闪烁
    if (root_) {
        root_->set_accepts_mouse(false);
    }
}

void UIManager::update(float dt) {
    // Widget 树更新（Button 状态动画等）
    const auto update_widget = [&](auto&& self, Widget* w) -> void {
        if (!w) return;
        w->update(dt);
        for (auto* child : w->children()) {
            self(self, child);
        }
    };
    update_widget(update_widget, root_);
    for (auto* modal : modals_) update_widget(update_widget, modal);

    // 驱动动画
    for (auto* anim : animations_) {
        if (anim) anim->update(dt);
    }
    animations_.erase(
        std::remove_if(animations_.begin(), animations_.end(),
                       [](Animation* a) {
                           if (!a) return true;
                           if (a->finished() || !a->valid()) {
                               delete a;
                               return true;
                           }
                           return false;
                       }),
        animations_.end());
}

void UIManager::draw_widget(Widget* widget) {
    if (!widget || !widget->visible()) return;

    // 样式表级联缓存：仅当 style_dirty_ 时重新解析
    if (global_style_ && widget->style_dirty_) {
        widget->cached_style_ = global_style_->resolve(widget);
        widget->style_ = widget->cached_style_;
        widget->style_dirty_ = false;
    }

    // 面板布局
    if (auto* panel = dynamic_cast<Panel*>(widget)) {
        panel->relayout();
    } else if (widget->layout_dirty_ && widget->parent() == nullptr) {
        widget->layout_dirty_ = false;
    }

    // ScrollView 裁剪：子控件完全在视口外时跳过
    if (widget->parent() && std::strcmp(widget->parent()->type_name(), "ScrollView") == 0) {
        const Rect& vp = widget->parent()->bounds();
        const Rect& b = widget->bounds();
        if (b.x + b.w < vp.x || b.x > vp.x + vp.w ||
            b.y + b.h < vp.y || b.y > vp.y + vp.h) {
            return;
        }
    }

    // 进入 ScrollView 时设置 scissor 裁剪，确保子控件不绘制到视口外
    bool is_scroll = dynamic_cast<ScrollView*>(widget) != nullptr;
    if (is_scroll && renderer2d_) {
        const Rect& b = widget->bounds();
        renderer2d_->set_scissor(
            static_cast<int>(b.x),
            static_cast<int>(b.y),
            static_cast<int>(b.w + 0.5f),
            static_cast<int>(b.h + 0.5f));
    }

    widget->draw(renderer_);
    for (auto* child : widget->children()) {
        draw_widget(child);
    }

    // 离开 ScrollView 时恢复全屏 scissor
    if (is_scroll && renderer2d_) {
        renderer2d_->reset_scissor();
    }
}

void UIManager::render() {
    if (!renderer_) return;
    int w = 0, h = 0;
    renderer_->window_size(w, h);
    if (w <= 0 || h <= 0) return;

    // 优先使用 UIRenderer（专用 UI 渲染管线）
    if (ui_renderer_ && ui_renderer_->initialized()) {
        ui_renderer_->begin_frame(static_cast<float>(w), static_cast<float>(h));

        // GenerateMesh: 遍历控件树生成顶点
        if (root_) {
            ui_renderer_->generate_mesh(root_, modals_);
        }

        ui_renderer_->end_frame();
        return;
    }

    // 回退到传统 2D 渲染器
    if (!renderer2d_) return;
    renderer2d_->begin_frame(static_cast<float>(w), static_cast<float>(h));
    // UI 坐标系：屏幕左上角为原点
    renderer2d_->set_camera(math::Vector2f::zero(), 1.0f, true);

    if (root_) {
        root_->set_bounds(Rect{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)});
        root_->layout_dirty_ = true;
        draw_widget(root_);
    }
    for (auto* modal : modals_) {
        modal->set_bounds(Rect{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)});
        modal->layout_dirty_ = true;
        draw_widget(modal);
    }

    renderer2d_->end_frame();
}

Widget* UIManager::hit_test_top(float x, float y) {
    const auto find = [&](auto&& self, Widget* w) -> Widget* {
        if (!w || !w->visible() || !w->hit_test(x, y)) return nullptr;
        for (auto it = w->children().rbegin(); it != w->children().rend(); ++it) {
            if (Widget* hit = self(self, *it)) return hit;
        }
        // 如果不接收鼠标事件，不返回自身（但其子控件仍可被命中）
        if (!w->accepts_mouse()) return nullptr;
        return w;
    };
    for (auto it = modals_.rbegin(); it != modals_.rend(); ++it) {
        if (Widget* hit = find(find, *it)) return hit;
    }
    return find(find, root_);
}

// ============================================================================
// JS 事件桥接：将 Widget 事件转发到 JS 回调函数
// ============================================================================

namespace {

// 根据 Widget 的 event_callbacks_ 查找对应事件名的 JS 回调并调用
static void fire_js_event(Widget* widget, const char* event_name) {
    if (!widget) return;
    const std::string& callback_name = widget->event_callback(event_name);
    if (callback_name.empty()) return;

    UIManager* mgr = UIManager::current();
    if (!mgr) return;

    // 通过 ScriptVM 调用全局 JS 函数
    // 注意：ScriptVM 的访问需要从 UIManager 获取
    // 这里借助 EngineBridge 的静态访问器
    ScriptVM* vm = EngineBridge::vm();
    if (!vm || !vm->initialized()) return;

    // 调用 JS 回调函数，传递 widget id 作为参数
    ScriptResult sr = vm->call_function(callback_name,
        {JSValueWrapper(widget->id().c_str())});
    if (!sr.success) {
        GLOG_WARN("[UI] JS callback '{}' failed for event '{}' on widget '{}': {}",
                  callback_name, event_name, widget->id(), sr.error_msg);
    }
}

} // namespace

// ============================================================================

void UIManager::update_hover(float x, float y) {
    Widget* hit = hit_test_top(x, y);
    if (hit == hovered_) return;
    if (hovered_) {
        hovered_->set_hovered(false);
        hovered_->on_hover.emit(hovered_, false);
        fire_js_event(hovered_, "hover_leave");
    }
    hovered_ = hit;
    if (hovered_) {
        hovered_->set_hovered(true);
        hovered_->on_hover.emit(hovered_, true);
        fire_js_event(hovered_, "hover_enter");
    }
}

void UIManager::on_mouse_move(float x, float y) {
    mouse_x_ = x;
    mouse_y_ = y;
    update_hover(x, y);

    if (pressed_widget_) {
        if (auto* slider = dynamic_cast<Slider*>(pressed_widget_)) {
            if (slider->is_vertical()) {
                slider->drag_to_vertical(y);
            } else {
                slider->drag_to(x);
            }
        } else if (auto* scroll = dynamic_cast<ScrollView*>(pressed_widget_)) {
            scroll->drag_to(x, y);
        }
    }
}

void UIManager::on_mouse_button(int button, bool down) {
    if (down) {
        pressed_button_ = button;
        pressed_widget_ = hit_test_top(mouse_x_, mouse_y_);
        if (pressed_widget_) {
            pressed_widget_->set_pressed(true);
            set_focus(pressed_widget_); // 所有可交互控件点击时获得焦点
            if (auto* slider = dynamic_cast<Slider*>(pressed_widget_)) {
                slider->drag_to(mouse_x_);
            } else if (auto* scroll = dynamic_cast<ScrollView*>(pressed_widget_)) {
                scroll->start_drag(mouse_x_, mouse_y_);
            } else if (auto* input = dynamic_cast<TextInput*>(pressed_widget_)) {
                input->set_cursor_from_screen_x(mouse_x_);
            } else if (auto* btn = dynamic_cast<Button*>(pressed_widget_)) {
                btn->on_pressed.emit();
            } else if (auto* combo = dynamic_cast<ComboBox*>(pressed_widget_)) {
                combo->toggle_popup();
            }
        }
        // 全局点击通知：先于状态修改，供弹出控件关闭自身
        const auto notify_global = [&](auto&& self, Widget* w) -> void {
            if (!w) return;
            w->on_global_click(hit_test_top(mouse_x_, mouse_y_));
            for (auto* child : w->children()) {
                self(self, child);
            }
        };
        notify_global(notify_global, root_);
        for (auto* modal : modals_) notify_global(notify_global, modal);
    } else {
        Widget* released = pressed_widget_;
        if (released) {
            released->set_pressed(false);
            if (auto* scroll = dynamic_cast<ScrollView*>(released)) {
                scroll->end_drag();
            }
            const bool same_widget = (hit_test_top(mouse_x_, mouse_y_) == released);
            if (same_widget) {
                if (auto* btn = dynamic_cast<Button*>(released)) {
                    btn->on_released.emit();
                }
                if (auto* cb = dynamic_cast<CheckBox*>(released)) {
                    cb->toggle();
                }
                if (auto* rb = dynamic_cast<RadioButton*>(released)) {
                    rb->toggle();
                }
                released->on_click.emit(released);
                // 桥接 JS 点击事件回调
                fire_js_event(released, "click");
            }
        }
        pressed_widget_ = nullptr;
        pressed_button_ = -1;
    }
}

void UIManager::on_keyboard(int key, bool down) {
    if (!down) return;
    if (focused_ && focused_->enabled()) {
        if (auto* input = dynamic_cast<TextInput*>(focused_)) {
            input->handle_key(key, down);
        }
    }
}

void UIManager::on_text_input(const char* utf8) {
    if (!utf8) return;
    if (focused_ && focused_->enabled()) {
        if (auto* input = dynamic_cast<TextInput*>(focused_)) {
            input->handle_text_input(utf8);
        }
    }
}

void UIManager::set_global_style(StyleSheet* style) {
    global_style_ = style;
}

void UIManager::set_focus(Widget* widget) {
    if (focused_ == widget) return;
    if (focused_) {
        focused_->set_focused(false);
        focused_->on_blur.emit(focused_);
        fire_js_event(focused_, "blur");
    }
    focused_ = widget;
    if (focused_) {
        focused_->set_focused(true);
        focused_->on_focus.emit(focused_);
        fire_js_event(focused_, "focus");
    }
}

void UIManager::push_modal(Widget* modal) {
    if (modal) modals_.push_back(modal);
}

void UIManager::pop_modal() {
    if (!modals_.empty()) {
        delete modals_.back();
        modals_.pop_back();
    }
}

void UIManager::add_animation(Animation* anim) {
    if (!anim) return;
    if (std::find(animations_.begin(), animations_.end(), anim) == animations_.end()) {
        animations_.push_back(anim);
    }
}

void UIManager::remove_animation(Animation* anim) {
    animations_.erase(std::remove(animations_.begin(), animations_.end(), anim),
                      animations_.end());
}

} // namespace GryceEngineUtils::ui
