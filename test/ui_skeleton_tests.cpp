#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "components/node_ui.h"
#include "scene/entity.h"
#include "ui/ui_drawlist.h"
#include "ui/ui_editor_widgets.h"
#include "ui/ui_extended_widgets.h"
#include "ui/ui_paint.h"
#include "ui/ui_runtime.h"
#include "ui/ui_texture.h"
#include "ui/ui_widgets.h"

namespace {

using gryce_engine::components::NodeUI;
using gryce_engine::components::UICheckBox;
using gryce_engine::components::UIColorPicker;
using gryce_engine::components::UICodeEditor;
using gryce_engine::components::UIConfirmDialog;
using gryce_engine::components::UIConsoleView;
using gryce_engine::components::UIButton;
using gryce_engine::components::UIDragFloat;
using gryce_engine::components::UIFileDialog;
using gryce_engine::components::UIAssetGrid;
using gryce_engine::components::UIMenu;
using gryce_engine::components::UIMultilineTextInput;
using gryce_engine::components::UINumberInput;
using gryce_engine::components::UIPropertyGrid;
using gryce_engine::components::UIPropertyEditor;
using gryce_engine::components::UIPropertyKind;
using gryce_engine::components::UIPropertyRow;
using gryce_engine::components::UIRangeSlider;
using gryce_engine::components::UISearchBox;
using gryce_engine::components::UITableView;
using gryce_engine::components::UIToast;
using gryce_engine::components::UITreeItem;
using gryce_engine::components::UITreeView;
using gryce_engine::components::UIVectorEditor;
using gryce_engine::components::UIImage;
using gryce_engine::components::UILabel;
using gryce_engine::components::UIPanel;
using gryce_engine::components::UIScrollView;
using gryce_engine::components::UISlider;
using gryce_engine::components::UITextInput;
using gryce_engine::components::UIViewportPanel;
using gryce_engine::components::UIComboBox;
using gryce_engine::components::UIDockHost;
using gryce_engine::components::UIDockPanel;
using gryce_engine::components::UIDockSide;
using gryce_engine::components::UIDragSource;
using gryce_engine::components::UIDropZone;
using gryce_engine::components::UIFloatingPanel;
using gryce_engine::components::UISplitter;
using gryce_engine::components::UITabBar;
using gryce_engine::math::Vector2f;
using gryce_engine::math::Vector3f;
using gryce_engine::ui::UIEvent;
using gryce_engine::ui::UIEventType;
using gryce_engine::ui::UIDrawList;
using gryce_engine::ui::UIInputFrame;
using gryce_engine::ui::UIRect;
using gryce_engine::ui::UIRuntime;
using gryce_engine::ui::UIThemeColor;
using gryce_engine::ui::UIThemePreset;

struct Case {
    std::string suite;
    std::string name;
    std::function<void()> fn;
};

std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

struct Registrar {
    Registrar(const char* suite, const char* name, void (*fn)()) {
        registry().push_back({suite, name, fn});
    }
};

int checks = 0;
int failed = 0;
int case_failed = 0;

void fail_at(const char* file, int line, const std::string& text) {
    ++failed;
    ++case_failed;
    std::printf("      FAIL %s:%d  %s\n", file, line, text.c_str());
}

} // namespace

#define TEST(suite, name)                                                        \
    static void suite##__##name##_impl();                                        \
    static Registrar suite##__##name##_reg(#suite, #name, &suite##__##name##_impl); \
    static void suite##__##name##_impl()

#define CHECK_TRUE(expr)                                                         \
    do {                                                                         \
        ++checks;                                                                \
        if (!(expr)) fail_at(__FILE__, __LINE__, "expected true: " #expr);       \
    } while (0)

#define CHECK_INT(actual, expected)                                              \
    do {                                                                         \
        ++checks;                                                                \
        const long long a = static_cast<long long>(actual);                      \
        const long long b = static_cast<long long>(expected);                    \
        if (a != b) {                                                            \
            char text[160];                                                      \
            std::snprintf(text, sizeof(text), "%s => %lld, expected %lld",       \
                          #actual, a, b);                                        \
            fail_at(__FILE__, __LINE__, text);                                   \
        }                                                                        \
    } while (0)

#define CHECK_NEAR(actual, expected)                                             \
    do {                                                                         \
        ++checks;                                                                \
        const double a = static_cast<double>(actual);                            \
        const double b = static_cast<double>(expected);                          \
        if (std::fabs(a - b) > 1e-5) {                                           \
            char text[180];                                                      \
            std::snprintf(text, sizeof(text), "%s => %.6f, expected %.6f",       \
                          #actual, a, b);                                        \
            fail_at(__FILE__, __LINE__, text);                                   \
        }                                                                        \
    } while (0)

namespace {

struct EventProbe : UIPanel {
    int enters = 0;
    int leaves = 0;
    int moves = 0;
    int downs = 0;
    int ups = 0;
    int clicks = 0;
    int focus_gained = 0;
    int focus_lost = 0;
    int key_downs = 0;
    int key_ups = 0;
    int characters = 0;
    bool handle_click = false;

    bool on_ui_event(const UIEvent& event) override {
        switch (event.type) {
            case UIEventType::PointerEnter: ++enters; break;
            case UIEventType::PointerLeave: ++leaves; break;
            case UIEventType::PointerMove: ++moves; break;
            case UIEventType::PointerDown: ++downs; break;
            case UIEventType::PointerUp: ++ups; break;
            case UIEventType::Click: ++clicks; return handle_click;
            case UIEventType::FocusGained: ++focus_gained; break;
            case UIEventType::FocusLost: ++focus_lost; break;
            case UIEventType::KeyDown: ++key_downs; break;
            case UIEventType::KeyUp: ++key_ups; break;
            case UIEventType::Character: ++characters; break;
        }
        return false;
    }
};

void attach(gryce_engine::scene::Entity& entity, NodeUI* parent, NodeUI* child) {
    if (parent) {
        CHECK_TRUE(entity.set_component_parent(child, parent));
    }
}

std::string test_clipboard;

std::string get_test_clipboard() {
    return test_clipboard;
}

void set_test_clipboard(const std::string& text) {
    test_clipboard = text;
}

UIInputFrame pointer_input(float x, float y) {
    UIInputFrame input;
    input.pointer = Vector2f(x, y);
    return input;
}

} // namespace

TEST(layout, column_padding_and_gap) {
    gryce_engine::scene::Entity entity("Layout");
    auto* root = entity.add_component<UIPanel>();
    auto* first = entity.add_component<UIPanel>();
    auto* second = entity.add_component<UILabel>();
    root->layout_mode = gryce_engine::components::UILayoutMode::Column;
    root->padding = 10.0f;
    root->gap = 4.0f;
    first->size = Vector2f(0.0f, 40.0f);
    second->size = Vector2f(0.0f, 20.0f);
    attach(entity, root, first);
    attach(entity, root, second);

    UIRuntime runtime;
    runtime.set_root(root);
    runtime.set_viewport({0.0f, 0.0f, 800.0f, 600.0f});
    runtime.update(pointer_input(-1.0f, -1.0f));

    const UIRect* root_rect = runtime.rect_for(root);
    const UIRect* first_rect = runtime.rect_for(first);
    const UIRect* second_rect = runtime.rect_for(second);
    CHECK_TRUE(root_rect && first_rect && second_rect);
    if (!root_rect || !first_rect || !second_rect) return;
    CHECK_NEAR(root_rect->w, 800.0f);
    CHECK_NEAR(first_rect->x, 10.0f);
    CHECK_NEAR(first_rect->y, 10.0f);
    CHECK_NEAR(first_rect->w, 780.0f);
    CHECK_NEAR(first_rect->h, 40.0f);
    CHECK_NEAR(second_rect->x, 10.0f);
    CHECK_NEAR(second_rect->y, 54.0f);
    CHECK_NEAR(second_rect->w, 780.0f);
    CHECK_NEAR(second_rect->h, 20.0f);
}

TEST(layout, manual_anchor_and_min_size) {
    gryce_engine::scene::Entity entity("Anchor");
    auto* root = entity.add_component<UIPanel>();
    auto* child = entity.add_component<UIPanel>();
    child->size = Vector2f(100.0f, 50.0f);
    child->min_size = Vector2f(120.0f, 60.0f);
    child->anchor_min = Vector2f(1.0f, 0.0f);
    child->anchor_max = Vector2f(1.0f, 0.0f);
    child->set_local_position(Vector3f(-130.0f, 20.0f, 0.0f));
    attach(entity, root, child);

    UIRuntime runtime;
    runtime.set_root(root);
    runtime.set_viewport({0.0f, 0.0f, 800.0f, 600.0f});
    runtime.update(pointer_input(-1.0f, -1.0f));

    const UIRect* rect = runtime.rect_for(child);
    CHECK_TRUE(rect != nullptr);
    if (!rect) return;
    CHECK_NEAR(rect->x, 670.0f);
    CHECK_NEAR(rect->y, 20.0f);
    CHECK_NEAR(rect->w, 120.0f);
    CHECK_NEAR(rect->h, 60.0f);
}

TEST(layout, world_space_layout_scale) {
    gryce_engine::scene::Entity entity("WorldScale");
    auto* root = entity.add_component<UIPanel>();
    root->size = Vector2f(100.0f, 50.0f);
    root->min_size = root->size;

    UIRuntime runtime;
    runtime.set_root(root);
    runtime.set_viewport({320.0f, 180.0f, 800.0f, 600.0f});
    runtime.set_layout_scale(2.0f);
    runtime.update(pointer_input(-1.0f, -1.0f));
    const UIRect* rect = runtime.rect_for(root);
    CHECK_TRUE(rect != nullptr);
    if (!rect) return;
    CHECK_NEAR(rect->x, 320.0f);
    CHECK_NEAR(rect->y, 180.0f);
    CHECK_NEAR(rect->w, 200.0f);
    CHECK_NEAR(rect->h, 100.0f);
}

TEST(input, hover_focus_bubbling_and_keyboard) {
    gryce_engine::scene::Entity entity("Input");
    auto* root = entity.add_component<UIPanel>();
    auto* outer = entity.add_component<EventProbe>();
    auto* inner = entity.add_component<EventProbe>();
    outer->size = Vector2f(300.0f, 200.0f);
    outer->focusable = true;
    inner->size = Vector2f(100.0f, 100.0f);
    attach(entity, root, outer);
    attach(entity, outer, inner);

    UIRuntime runtime;
    runtime.set_root(root);
    runtime.set_viewport({0.0f, 0.0f, 800.0f, 600.0f});

    UIInputFrame first = pointer_input(20.0f, 20.0f);
    first.pointer_down[0] = true;
    first.pointer_pressed[0] = true;
    runtime.update(first);
    CHECK_TRUE(runtime.hovered() == inner);
    CHECK_TRUE(runtime.active() == inner);
    CHECK_TRUE(runtime.focused() == outer);
    CHECK_TRUE(runtime.wants_mouse());
    CHECK_TRUE(runtime.wants_keyboard());
    CHECK_INT(inner->enters, 1);
    CHECK_INT(inner->downs, 1);
    CHECK_INT(outer->downs, 1);
    CHECK_INT(outer->focus_gained, 1);

    UIInputFrame second = pointer_input(20.0f, 20.0f);
    second.pointer_down[0] = true;
    second.key_down.push_back({65, false});
    second.characters.push_back('A');
    runtime.update(second);
    CHECK_INT(outer->key_downs, 1);
    CHECK_INT(outer->characters, 1);

    UIInputFrame third = pointer_input(20.0f, 20.0f);
    third.pointer_released[0] = true;
    third.key_up.push_back(65);
    runtime.update(third);
    CHECK_INT(inner->ups, 1);
    CHECK_INT(outer->ups, 1);
    CHECK_INT(inner->clicks, 1);
    CHECK_INT(outer->clicks, 1);
    CHECK_INT(outer->key_ups, 1);
    CHECK_TRUE(runtime.active() == nullptr);

    UIInputFrame leave = pointer_input(500.0f, 500.0f);
    runtime.update(leave);
    CHECK_TRUE(runtime.hovered() == root);
    CHECK_INT(inner->leaves, 1);
}

TEST(drawlist, panel_text_and_clip_order) {
    gryce_engine::scene::Entity entity("Draw");
    auto* root = entity.add_component<UIPanel>();
    auto* label = entity.add_component<UILabel>();
    auto* image = entity.add_component<UIImage>();
    root->clip_contents = true;
    label->text = "GryceUI";
    label->font_size = 18.0f;
    label->size = Vector2f(120.0f, 30.0f);
    image->size = Vector2f(32.0f, 32.0f);
    image->texture_index = 7;
    image->texture_generation = 3;
    attach(entity, root, image);
    attach(entity, root, label);

    UIRuntime runtime;
    runtime.set_root(root);
    runtime.set_viewport({0.0f, 0.0f, 800.0f, 600.0f});
    runtime.update(pointer_input(-1.0f, -1.0f));

    gryce_engine::ui::UIDrawList draw_list;
    runtime.build_draw_list(draw_list);
    // 有贴图时 UIImage 只发一条 Image（不再多铺一层占位底）。
    CHECK_INT(draw_list.size(), 5);
    if (draw_list.size() != 5) return;

    const auto& commands = draw_list.commands();
    CHECK_TRUE(commands[0].type == gryce_engine::ui::UIDrawCommandType::Rect);
    CHECK_TRUE(commands[1].type == gryce_engine::ui::UIDrawCommandType::PushClip);
    CHECK_TRUE(commands[2].type == gryce_engine::ui::UIDrawCommandType::Image);
    CHECK_TRUE(commands[3].type == gryce_engine::ui::UIDrawCommandType::Text);
    CHECK_TRUE(commands[4].type == gryce_engine::ui::UIDrawCommandType::PopClip);
    CHECK_INT(commands[2].texture_index, 7);
    CHECK_INT(commands[2].texture_generation, 3);
    CHECK_TRUE(commands[3].text == "GryceUI");
    CHECK_NEAR(commands[3].font_size, 18.0f);
    CHECK_INT(gryce_engine::ui::ui_color(1.0f, 0.0f, 0.0f, 1.0f), 0xFF0000FFu);
}

TEST(drawlist, viewport_texture_and_letterbox) {
    gryce_engine::scene::Entity entity("Viewport");
    auto* root = entity.add_component<UIPanel>();
    auto* viewport = entity.add_component<UIViewportPanel>();
    viewport->size = Vector2f(200.0f, 200.0f);
    viewport->texture = {7, 3, 320, 180};
    attach(entity, root, viewport);

    UIRuntime runtime;
    runtime.set_root(root);
    runtime.set_viewport({0.0f, 0.0f, 800.0f, 600.0f});
    runtime.update(pointer_input(-1.0f, -1.0f));
    gryce_engine::ui::UIDrawList draw_list;
    runtime.build_draw_list(draw_list);
    CHECK_INT(draw_list.size(), 5);  // root rect, letterbox, image, clip push/pop
    if (draw_list.size() != 5) return;
    const auto& commands = draw_list.commands();
    CHECK_TRUE(commands[1].type == gryce_engine::ui::UIDrawCommandType::Rect);
    CHECK_TRUE(commands[2].type == gryce_engine::ui::UIDrawCommandType::Image);
    CHECK_TRUE(commands[3].type == gryce_engine::ui::UIDrawCommandType::PushClip);
    CHECK_TRUE(commands[4].type == gryce_engine::ui::UIDrawCommandType::PopClip);
    CHECK_INT(commands[2].texture_index, 7);
    CHECK_INT(commands[2].texture_generation, 3);
    CHECK_NEAR(commands[2].rect.w, 200.0f);
    CHECK_NEAR(commands[2].rect.h, 112.5f);
    CHECK_NEAR(commands[2].rect.y, 43.75f);
}

TEST(runtime, visibility_and_clip_control_hit_testing) {
    gryce_engine::scene::Entity entity("Runtime");
    auto* root = entity.add_component<UIPanel>();
    auto* child = entity.add_component<EventProbe>();
    root->size = Vector2f(200.0f, 200.0f);
    root->clip_contents = true;
    root->receives_input = false;
    child->size = Vector2f(80.0f, 40.0f);
    attach(entity, root, child);

    UIRuntime runtime;
    runtime.set_root(root);
    runtime.set_viewport({0.0f, 0.0f, 800.0f, 600.0f});
    runtime.update(pointer_input(20.0f, 20.0f));
    CHECK_TRUE(runtime.hovered() == child);

    child->visible = false;
    runtime.update(pointer_input(20.0f, 20.0f));
    CHECK_TRUE(runtime.hovered() == nullptr);
    CHECK_TRUE(runtime.rect_for(child) == nullptr);

    child->visible = true;
    runtime.update(pointer_input(240.0f, 240.0f));
    CHECK_TRUE(runtime.hovered() == nullptr);

    runtime.update(pointer_input(180.0f, 180.0f));
    CHECK_TRUE(runtime.hovered() == nullptr);
    CHECK_TRUE(runtime.rect_for(root) != nullptr);

    UIInputFrame serial_frame = pointer_input(20.0f, 20.0f);
    serial_frame.frame_serial = 7;
    serial_frame.pointer_down[0] = true;
    serial_frame.pointer_pressed[0] = true;
    runtime.update(serial_frame);
    const int enters_once = child->enters;
    const int downs_once = child->downs;
    runtime.update(serial_frame);
    CHECK_INT(child->enters, enters_once);
    CHECK_INT(child->downs, downs_once);

    runtime.clear();
    CHECK_TRUE(runtime.root() == nullptr);
    CHECK_TRUE(runtime.rect_for(root) == nullptr);
}

TEST(widgets, interactive_controls_and_text_editing) {
    gryce_engine::scene::Entity entity("Widgets");
    auto* root = entity.add_component<UIPanel>();
    auto* button = entity.add_component<UIButton>();
    auto* check = entity.add_component<UICheckBox>();
    auto* slider = entity.add_component<UISlider>();
    auto* text = entity.add_component<UITextInput>();
    auto* scroll = entity.add_component<UIScrollView>();
    scroll->size = Vector2f(200.0f, 80.0f);
    scroll->content_height = 200.0f;

    button->set_local_position(Vector3f(10.0f, 10.0f, 0.0f));
    check->set_local_position(Vector3f(10.0f, 50.0f, 0.0f));
    slider->set_local_position(Vector3f(10.0f, 80.0f, 0.0f));
    text->set_local_position(Vector3f(10.0f, 110.0f, 0.0f));
    scroll->set_local_position(Vector3f(10.0f, 150.0f, 0.0f));
    attach(entity, root, button);
    attach(entity, root, check);
    attach(entity, root, slider);
    attach(entity, root, text);
    attach(entity, root, scroll);

    UIRuntime runtime;
    runtime.set_root(root);
    runtime.set_viewport({0.0f, 0.0f, 800.0f, 600.0f});

    UIInputFrame down = pointer_input(20.0f, 20.0f);
    down.pointer_down[0] = true;
    down.pointer_pressed[0] = true;
    runtime.update(down);
    UIInputFrame up = pointer_input(20.0f, 20.0f);
    up.pointer_released[0] = true;
    runtime.update(up);
    CHECK_TRUE(button->clicked);

    UIInputFrame check_click = pointer_input(20.0f, 60.0f);
    check_click.pointer_down[0] = true;
    check_click.pointer_pressed[0] = true;
    runtime.update(check_click);
    UIInputFrame check_up = pointer_input(20.0f, 60.0f);
    check_up.pointer_released[0] = true;
    runtime.update(check_up);
    CHECK_TRUE(check->checked);

    UIInputFrame slider_down = pointer_input(110.0f, 90.0f);
    slider_down.pointer_down[0] = true;
    slider_down.pointer_pressed[0] = true;
    runtime.update(slider_down);
    CHECK_NEAR(slider->value, 0.5f);
    UIInputFrame slider_drag = pointer_input(160.0f, 90.0f);
    slider_drag.pointer_down[0] = true;
    runtime.update(slider_drag);
    CHECK_NEAR(slider->value, 0.75f);
    UIInputFrame slider_up = pointer_input(160.0f, 90.0f);
    slider_up.pointer_released[0] = true;
    runtime.update(slider_up);

    UIInputFrame text_focus = pointer_input(20.0f, 120.0f);
    text_focus.pointer_down[0] = true;
    text_focus.pointer_pressed[0] = true;
    runtime.update(text_focus);
    UIInputFrame preedit = pointer_input(20.0f, 120.0f);
    preedit.pointer_down[0] = true;
    preedit.preedit = "ni";
    runtime.update(preedit);
    gryce_engine::ui::UIDrawList input_draw;
    runtime.build_draw_list(input_draw);
    bool saw_preedit = false;
    for (const auto& command : input_draw.commands()) {
        saw_preedit = saw_preedit || command.text == "ni";
    }
    CHECK_TRUE(saw_preedit);

    UIInputFrame type = pointer_input(20.0f, 120.0f);
    type.pointer_down[0] = true;
    type.characters = {'A', 0x4E2D};
    runtime.update(type);
    CHECK_TRUE(text->text == "A中");
    UIInputFrame backspace = pointer_input(20.0f, 120.0f);
    backspace.pointer_down[0] = true;
    backspace.key_down.push_back({static_cast<int>(gryce_engine::ui::UIKey::Backspace), false});
    runtime.update(backspace);
    CHECK_TRUE(text->text == "A");

    UIInputFrame tab = pointer_input(20.0f, 120.0f);
    tab.pointer_down[0] = true;
    tab.key_down.push_back({static_cast<int>(gryce_engine::ui::UIKey::Tab), false});
    runtime.update(tab);
    CHECK_TRUE(runtime.focused() == button);

    UIInputFrame wheel = pointer_input(20.0f, 170.0f);
    wheel.wheel = Vector2f(0.0f, -2.0f);
    runtime.update(wheel);
    CHECK_NEAR(scroll->scroll_y, 48.0f);
    CHECK_NEAR(scroll->content_offset().y, -48.0f);
}

TEST(widgets, dock_splitter_tabs_combo_and_shortcut) {
    gryce_engine::scene::Entity entity("DockWidgets");
    auto* dock = entity.add_component<UIDockHost>();
    auto* top = entity.add_component<UIDockPanel>();
    auto* left = entity.add_component<UIDockPanel>();
    auto* right = entity.add_component<UIDockPanel>();
    auto* bottom = entity.add_component<UIDockPanel>();
    auto* center = entity.add_component<UIDockPanel>();
    top->side = UIDockSide::Top;
    left->side = UIDockSide::Left;
    right->side = UIDockSide::Right;
    bottom->side = UIDockSide::Bottom;
    center->side = UIDockSide::Center;
    attach(entity, dock, top);
    attach(entity, dock, left);
    attach(entity, dock, right);
    attach(entity, dock, bottom);
    attach(entity, dock, center);

    UIRuntime runtime;
    runtime.set_root(dock);
    runtime.set_viewport({0.0f, 0.0f, 1000.0f, 600.0f});
    runtime.update(pointer_input(-1.0f, -1.0f));
    const UIRect* cr_ptr = runtime.rect_for(center);
    CHECK_TRUE(cr_ptr != nullptr);
    UIRect center_rect;
    if (cr_ptr) {
        center_rect = *cr_ptr;
        CHECK_NEAR(center_rect.x, 262.0f);
        CHECK_NEAR(center_rect.y, 38.0f);
        CHECK_NEAR(center_rect.w, 456.0f);
        CHECK_NEAR(center_rect.h, 420.0f);
    }

    auto* splitter = entity.add_component<UISplitter>();
    splitter->set_local_position(Vector3f(100.0f, 0.0f, 0.0f));
    attach(entity, center, splitter);
    runtime.update(pointer_input(-1.0f, -1.0f));
    const UIRect* splitter_rect = runtime.rect_for(splitter);
    CHECK_TRUE(splitter_rect != nullptr);
    if (!splitter_rect) return;
    const float split_x = splitter_rect->x + splitter_rect->w * 0.5f;
    const float split_y = splitter_rect->y + 10.0f;
    UIInputFrame down = pointer_input(split_x, split_y);
    down.pointer_down[0] = true;
    down.pointer_pressed[0] = true;
    runtime.update(down);
    const float drag_x = center_rect.x + center_rect.w * 0.3f;
    UIInputFrame drag = pointer_input(drag_x, split_y);
    drag.pointer_down[0] = true;
    runtime.update(drag);
    CHECK_NEAR(splitter->ratio, 0.3f);
    UIInputFrame split_up = pointer_input(drag_x, split_y);
    split_up.pointer_released[0] = true;
    runtime.update(split_up);

    auto* tabs = entity.add_component<UITabBar>();
    tabs->items = "Scene\nGame\nAsset";
    tabs->set_local_position(Vector3f(0.0f, 0.0f, 0.0f));
    tabs->size = Vector2f(440.0f, 28.0f);
    attach(entity, center, tabs);
    runtime.update(pointer_input(-1.0f, -1.0f));
    const UIRect* tab_rect = runtime.rect_for(tabs);
    CHECK_TRUE(tab_rect != nullptr);
    if (!tab_rect) return;
    const UIRect tab_area = *tab_rect;
    runtime.update(pointer_input(tab_area.x + tab_area.w * 0.8f,
                                 tab_area.y + tab_area.h * 0.5f));
    CHECK_TRUE(runtime.hovered() == tabs);
    UIInputFrame tab_click = pointer_input(
        tab_area.x + tab_area.w * 0.8f, tab_area.y + tab_area.h * 0.5f);
    tab_click.pointer_down[0] = true;
    tab_click.pointer_pressed[0] = true;
    runtime.update(tab_click);
    CHECK_TRUE(runtime.active() == tabs);
    CHECK_INT(tabs->selected, 2);
    UIInputFrame tab_up = tab_click;
    tab_up.pointer_down[0] = false;
    tab_up.pointer_pressed[0] = false;
    tab_up.pointer_released[0] = true;
    runtime.update(tab_up);

    auto* combo = entity.add_component<UIComboBox>();
    combo->set_local_position(Vector3f(0.0f, 200.0f, 0.0f));
    attach(entity, center, combo);
    runtime.update(pointer_input(-1.0f, -1.0f));
    const UIRect* combo_rect = runtime.rect_for(combo);
    CHECK_TRUE(combo_rect != nullptr);
    if (!combo_rect) return;
    UIInputFrame combo_open = pointer_input(
        combo_rect->x + combo_rect->w * 0.5f,
        combo_rect->y + combo_rect->h * 0.5f);
    combo_open.pointer_down[0] = true;
    combo_open.pointer_pressed[0] = true;
    runtime.update(combo_open);
    UIInputFrame combo_up = combo_open;
    combo_up.pointer_down[0] = false;
    combo_up.pointer_pressed[0] = false;
    combo_up.pointer_released[0] = true;
    runtime.update(combo_up);
    CHECK_TRUE(combo->open);

    runtime.add_shortcut(290, 1, 42);
    UIInputFrame shortcut = pointer_input(-1.0f, -1.0f);
    shortcut.modifiers = 1;
    shortcut.key_down.push_back({290, false});
    runtime.update(shortcut);
    CHECK_INT(runtime.take_shortcut_command(), 42);
}

TEST(widgets, text_selection_and_clipboard) {
    gryce_engine::ui::set_clipboard_callbacks(get_test_clipboard, set_test_clipboard);
    test_clipboard.clear();

    gryce_engine::scene::Entity entity("Clipboard");
    auto* root = entity.add_component<UIPanel>();
    auto* input = entity.add_component<UITextInput>();
    input->set_local_position(Vector3f(10.0f, 10.0f, 0.0f));
    attach(entity, root, input);

    UIRuntime runtime;
    runtime.set_root(root);
    runtime.set_viewport({0.0f, 0.0f, 800.0f, 600.0f});
    UIInputFrame focus = pointer_input(20.0f, 20.0f);
    focus.pointer_down[0] = true;
    focus.pointer_pressed[0] = true;
    runtime.update(focus);
    CHECK_TRUE(runtime.focused() == input);

    UIInputFrame type = pointer_input(20.0f, 20.0f);
    type.pointer_down[0] = true;
    type.characters = {'A', 'B'};
    runtime.update(type);
    CHECK_TRUE(input->text == "AB");

    UIInputFrame copy = pointer_input(20.0f, 20.0f);
    copy.pointer_down[0] = true;
    copy.modifiers = 1;
    copy.key_down.push_back({static_cast<int>(gryce_engine::ui::UIKey::A), false});
    copy.key_down.push_back({static_cast<int>(gryce_engine::ui::UIKey::C), false});
    runtime.update(copy);
    CHECK_TRUE(test_clipboard == "AB");

    test_clipboard = "Z";
    UIInputFrame paste = pointer_input(20.0f, 20.0f);
    paste.pointer_down[0] = true;
    paste.modifiers = 1;
    paste.key_down.push_back({static_cast<int>(gryce_engine::ui::UIKey::V), false});
    runtime.update(paste);
    CHECK_TRUE(input->text == "Z");
}

TEST(widgets, drag_drop_and_floating_panel) {
    gryce_engine::scene::Entity entity("DragWidgets");
    auto* root = entity.add_component<UIPanel>();
    auto* source = entity.add_component<UIDragSource>();
    auto* target = entity.add_component<UIDropZone>();
    auto* floating = entity.add_component<UIFloatingPanel>();
    source->size = Vector2f(100.0f, 30.0f);
    source->payload = "asset:cube.obj";
    source->set_local_position(Vector3f(10.0f, 10.0f, 0.0f));
    target->size = Vector2f(200.0f, 100.0f);
    target->set_local_position(Vector3f(300.0f, 100.0f, 0.0f));
    target->accepted_prefix = "asset:";
    floating->size = Vector2f(160.0f, 100.0f);
    floating->set_local_position(Vector3f(500.0f, 20.0f, 0.0f));
    attach(entity, root, source);
    attach(entity, root, target);
    attach(entity, root, floating);

    UIRuntime runtime;
    runtime.set_root(root);
    runtime.set_viewport({0.0f, 0.0f, 800.0f, 600.0f});
    UIInputFrame down = pointer_input(20.0f, 20.0f);
    down.pointer_down[0] = true;
    down.pointer_pressed[0] = true;
    runtime.update(down);
    UIInputFrame move = pointer_input(350.0f, 140.0f);
    move.pointer_down[0] = true;
    runtime.update(move);
    UIInputFrame release = pointer_input(350.0f, 140.0f);
    release.pointer_released[0] = true;
    runtime.update(release);
    CHECK_TRUE(target->last_payload == "asset:cube.obj");

    const Vector3f before = floating->local_position();
    UIInputFrame float_down = pointer_input(520.0f, 30.0f);
    float_down.pointer_down[0] = true;
    float_down.pointer_pressed[0] = true;
    runtime.update(float_down);
    UIInputFrame float_move = pointer_input(560.0f, 70.0f);
    float_move.pointer_down[0] = true;
    runtime.update(float_move);
    CHECK_TRUE(floating->local_position().x > before.x);
    CHECK_TRUE(floating->local_position().y > before.y);
}

TEST(widgets, numeric_and_color_controls) {
    gryce_engine::scene::Entity entity("NumericWidgets");
    auto* root = entity.add_component<UIPanel>();
    auto* number = entity.add_component<UINumberInput>();
    auto* drag = entity.add_component<UIDragFloat>();
    auto* vector = entity.add_component<UIVectorEditor>();
    auto* picker = entity.add_component<UIColorPicker>();
    auto* range = entity.add_component<UIRangeSlider>();
    number->set_local_position(Vector3f(10.0f, 10.0f, 0.0f));
    drag->set_local_position(Vector3f(10.0f, 50.0f, 0.0f));
    vector->set_local_position(Vector3f(10.0f, 90.0f, 0.0f));
    picker->set_local_position(Vector3f(10.0f, 130.0f, 0.0f));
    range->set_local_position(Vector3f(10.0f, 290.0f, 0.0f));
    attach(entity, root, number);
    attach(entity, root, drag);
    attach(entity, root, vector);
    attach(entity, root, picker);
    attach(entity, root, range);

    UIRuntime runtime;
    runtime.set_root(root);
    runtime.set_viewport({0.0f, 0.0f, 800.0f, 600.0f});
    runtime.update(pointer_input(-1.0f, -1.0f));

    const UIRect* drag_rect = runtime.rect_for(drag);
    CHECK_TRUE(drag_rect != nullptr);
    if (!drag_rect) return;
    const UIRect drag_area = *drag_rect;
    UIInputFrame down = pointer_input(drag_area.x + 20.0f,
                                      drag_area.y + drag_area.h * 0.5f);
    down.pointer_down[0] = true;
    down.pointer_pressed[0] = true;
    runtime.update(down);
    UIInputFrame move = pointer_input(drag_area.x + 60.0f,
                                      drag_area.y + drag_area.h * 0.5f);
    move.pointer_down[0] = true;
    runtime.update(move);
    CHECK_NEAR(drag->value, 0.8f);
    UIInputFrame up = pointer_input(drag_area.x + 60.0f,
                                    drag_area.y + drag_area.h * 0.5f);
    up.pointer_released[0] = true;
    runtime.update(up);

    const UIRect* vector_rect = runtime.rect_for(vector);
    CHECK_TRUE(vector_rect != nullptr);
    if (!vector_rect) return;
    const UIRect vector_area = *vector_rect;
    const float lane_w = vector_area.w / 3.0f;
    UIInputFrame vector_down = pointer_input(vector_area.x + lane_w * 1.5f,
                                             vector_area.y + 10.0f);
    vector_down.pointer_down[0] = true;
    vector_down.pointer_pressed[0] = true;
    runtime.update(vector_down);
    UIInputFrame vector_move = pointer_input(vector_area.x + lane_w * 1.5f + 50.0f,
                                             vector_area.y + 10.0f);
    vector_move.pointer_down[0] = true;
    runtime.update(vector_move);
    CHECK_INT(vector->active_component, 1);
    CHECK_TRUE(vector->values[1] > 0.5f);

    const UIRect* picker_rect = runtime.rect_for(picker);
    CHECK_TRUE(picker_rect != nullptr);
    if (!picker_rect) return;
    UIInputFrame pick = pointer_input(picker_rect->x + picker_rect->w * 0.5f,
                                      picker_rect->bottom() - 8.0f);
    pick.pointer_down[0] = true;
    pick.pointer_pressed[0] = true;
    runtime.update(pick);
    CHECK_TRUE(picker->hue > 0.45f);
    CHECK_TRUE(picker->hue < 0.55f);
    CHECK_TRUE(picker->color_changed);

    const UIRect* range_rect = runtime.rect_for(range);
    CHECK_TRUE(range_rect != nullptr);
    if (!range_rect) return;
    UIInputFrame range_pick = pointer_input(range_rect->x + range_rect->w * 0.9f,
                                            range_rect->y + 10.0f);
    range_pick.pointer_down[0] = true;
    range_pick.pointer_pressed[0] = true;
    runtime.update(range_pick);
    CHECK_NEAR(range->high, 0.9f);

    number->step = 0.001f;
    number->set_value(12.345f, true);
    CHECK_NEAR(number->value, 12.345f);
    CHECK_TRUE(number->value_changed);
    CHECK_TRUE(number->text.find("12.345") != std::string::npos);
}

TEST(widgets, menus_and_multiline_text) {
    gryce_engine::scene::Entity entity("MenuWidgets");
    auto* root = entity.add_component<UIPanel>();
    auto* menu = entity.add_component<UIMenu>();
    auto* text = entity.add_component<UIMultilineTextInput>();
    menu->set_local_position(Vector3f(20.0f, 20.0f, 0.0f));
    text->set_local_position(Vector3f(20.0f, 180.0f, 0.0f));
    attach(entity, root, menu);
    attach(entity, root, text);

    UIRuntime runtime;
    runtime.set_root(root);
    runtime.set_viewport({0.0f, 0.0f, 800.0f, 600.0f});
    menu->set_open(true);
    runtime.update(pointer_input(-1.0f, -1.0f));
    const UIRect* menu_rect = runtime.rect_for(menu);
    CHECK_TRUE(menu_rect != nullptr);
    if (!menu_rect) return;
    UIInputFrame pick = pointer_input(menu_rect->x + 20.0f,
                                      menu_rect->y + menu->padding +
                                          menu->row_height * 1.5f);
    pick.pointer_down[0] = true;
    pick.pointer_pressed[0] = true;
    runtime.update(pick);
    UIInputFrame release = pick;
    release.pointer_down[0] = false;
    release.pointer_pressed[0] = false;
    release.pointer_released[0] = true;
    runtime.update(release);
    CHECK_INT(menu->command, 1);
    CHECK_TRUE(!menu->visible);

    const UIRect* text_rect = runtime.rect_for(text);
    CHECK_TRUE(text_rect != nullptr);
    if (!text_rect) return;
    const UIRect text_area = *text_rect;
    UIInputFrame focus = pointer_input(text_area.x + 10.0f, text_area.y + 10.0f);
    focus.pointer_down[0] = true;
    focus.pointer_pressed[0] = true;
    runtime.update(focus);
    UIInputFrame type = pointer_input(text_area.x + 10.0f, text_area.y + 10.0f);
    type.pointer_down[0] = true;
    type.characters = {'A', 'B'};
    runtime.update(type);
    UIInputFrame newline = pointer_input(text_area.x + 10.0f, text_area.y + 10.0f);
    newline.pointer_down[0] = true;
    newline.key_down.push_back({static_cast<int>(gryce_engine::ui::UIKey::Enter), false});
    runtime.update(newline);
    UIInputFrame type_c = pointer_input(text_area.x + 10.0f, text_area.y + 10.0f);
    type_c.pointer_down[0] = true;
    type_c.characters = {'C'};
    runtime.update(type_c);
    CHECK_TRUE(text->text == "AB\nC");
}

TEST(widgets, data_views_and_dialogs) {
    gryce_engine::scene::Entity entity("DataWidgets");
    auto* root = entity.add_component<UIPanel>();
    auto* tree = entity.add_component<UITreeView>();
    auto* table = entity.add_component<UITableView>();
    auto* properties = entity.add_component<UIPropertyGrid>();
    auto* assets = entity.add_component<UIAssetGrid>();
    auto* dialog = entity.add_component<UIConfirmDialog>();
    auto* toast = entity.add_component<UIToast>();

    tree->items = {
        {"Scene", 0, true, true, true},
        {"Camera", 1, true, false, true},
        {"Cube", 1, true, false, true},
    };
    tree->set_local_position(Vector3f(10.0f, 10.0f, 0.0f));
    table->set_local_position(Vector3f(320.0f, 10.0f, 0.0f));
    properties->set_local_position(Vector3f(10.0f, 290.0f, 0.0f));
    assets->set_local_position(Vector3f(350.0f, 290.0f, 0.0f));
    dialog->set_local_position(Vector3f(120.0f, 80.0f, 0.0f));
    toast->set_local_position(Vector3f(420.0f, 20.0f, 0.0f));
    attach(entity, root, tree);
    attach(entity, root, table);
    attach(entity, root, properties);
    attach(entity, root, assets);
    attach(entity, root, dialog);
    attach(entity, root, toast);

    UIRuntime runtime;
    runtime.set_root(root);
    runtime.set_viewport({0.0f, 0.0f, 1000.0f, 700.0f});
    runtime.update(pointer_input(-1.0f, -1.0f));

    const UIRect* tree_rect = runtime.rect_for(tree);
    CHECK_TRUE(tree_rect != nullptr);
    if (!tree_rect) return;
    UIInputFrame fold = pointer_input(tree_rect->x + 8.0f, tree_rect->y + 12.0f);
    fold.pointer_down[0] = true;
    fold.pointer_pressed[0] = true;
    runtime.update(fold);
    CHECK_TRUE(!tree->items[0].expanded);

    const UIRect* table_rect = runtime.rect_for(table);
    CHECK_TRUE(table_rect != nullptr);
    if (!table_rect) return;
    UIInputFrame table_pick = pointer_input(table_rect->x + 20.0f,
                                            table_rect->y + table->header_height + 12.0f);
    table_pick.pointer_down[0] = true;
    table_pick.pointer_pressed[0] = true;
    runtime.update(table_pick);
    CHECK_INT(table->selected, 0);

    const UIRect* asset_rect = runtime.rect_for(assets);
    CHECK_TRUE(asset_rect != nullptr);
    if (!asset_rect) return;
    UIInputFrame asset_pick = pointer_input(asset_rect->x + 20.0f,
                                            asset_rect->y + 20.0f);
    asset_pick.pointer_down[0] = true;
    asset_pick.pointer_pressed[0] = true;
    runtime.update(asset_pick);
    CHECK_INT(assets->selected, 0);

    dialog->set_open(true);
    runtime.update(pointer_input(-1.0f, -1.0f));
    const UIRect* dialog_rect = runtime.rect_for(dialog);
    CHECK_TRUE(dialog_rect != nullptr);
    if (!dialog_rect) return;
    UIInputFrame confirm = pointer_input(dialog_rect->right() - 60.0f,
                                         dialog_rect->bottom() - 24.0f);
    confirm.pointer_down[0] = true;
    confirm.pointer_pressed[0] = true;
    runtime.update(confirm);
    UIInputFrame confirm_release = confirm;
    confirm_release.pointer_down[0] = false;
    confirm_release.pointer_pressed[0] = false;
    confirm_release.pointer_released[0] = true;
    runtime.update(confirm_release);
    CHECK_INT(dialog->result, 1);
    CHECK_TRUE(dialog->closed);

    toast->show("Saved", 0.5f);
    toast->update(0.6f);
    CHECK_TRUE(!toast->visible);
    CHECK_TRUE(toast->closed);

    nlohmann::json saved;
    tree->serialize(saved);
    UITreeView copy;
    copy.deserialize(saved);
    CHECK_INT(copy.items.size(), 3);
    CHECK_TRUE(!copy.items[0].expanded);
}

TEST(widgets, hidden_popup_blocks_children_until_opened) {
    gryce_engine::scene::Entity entity("HiddenPopup");
    auto* root = entity.add_component<UIPanel>();
    auto* popup = entity.add_component<UIFloatingPanel>();
    auto* button = entity.add_component<UIButton>();
    popup->set_local_position(Vector3f(20.0f, 20.0f, 0.0f));
    popup->size = Vector2f(240.0f, 160.0f);
    button->set_local_position(Vector3f(10.0f, 40.0f, 0.0f));
    button->size = Vector2f(100.0f, 28.0f);
    attach(entity, root, popup);
    attach(entity, popup, button);
    popup->visible = false;
    popup->receives_input = false;

    UIRuntime runtime;
    runtime.set_root(root);
    runtime.set_viewport({0.0f, 0.0f, 640.0f, 480.0f});
    runtime.update(pointer_input(40.0f, 70.0f));
    CHECK_TRUE(runtime.hovered() != button);

    popup->visible = true;
    popup->receives_input = true;
    runtime.update(pointer_input(41.0f, 71.0f));
    CHECK_TRUE(runtime.hovered() == button);
}

TEST(widgets, code_editor_and_console_document_flow) {
    gryce_engine::scene::Entity entity("DocumentWidgets");
    auto* root = entity.add_component<UIPanel>();
    auto* editor = entity.add_component<UICodeEditor>();
    auto* console = entity.add_component<UIConsoleView>();
    editor->set_local_position(Vector3f(10.0f, 10.0f, 0.0f));
    console->set_local_position(Vector3f(10.0f, 260.0f, 0.0f));
    attach(entity, root, editor);
    attach(entity, root, console);

    editor->set_code("const value = 1;\nconsole.log(value);");
    CHECK_INT(editor->current_line, 2);
    CHECK_TRUE(!editor->dirty);

    const auto path = std::filesystem::temp_directory_path() /
                      "gryce_ui_code_editor_test.js";
    CHECK_TRUE(editor->save_file(path.string()));
    CHECK_TRUE(!editor->dirty);
    editor->set_code({});
    CHECK_TRUE(editor->load_file(path.string()));
    CHECK_TRUE(editor->text.find("console.log") != std::string::npos);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    console->clear();
    console->append("[info] build started");
    console->append("[error] test failure");
    CHECK_TRUE(console->items.find("build started") != std::string::npos);
    CHECK_INT(console->selected, 1);
    console->max_lines = 1;
    console->append("[info] done");
    CHECK_TRUE(console->items.find("test failure") == std::string::npos);
}

TEST(widgets, reflection_property_editor_commit_flow) {
    gryce_engine::scene::Entity entity("PropertyEditor");
    auto* root = entity.add_component<UIPanel>();
    auto* editor = entity.add_component<UIPropertyEditor>();
    editor->set_local_position(Vector3f(10.0f, 10.0f, 0.0f));
    editor->size = Vector2f(320.0f, 180.0f);
    editor->rows = {
        {"Enabled", "true", "enabled", 17, UIPropertyKind::Boolean, 0.0f, 1.0f, false},
        {"Intensity", "1.0", "intensity", 17, UIPropertyKind::Float, 0.0f, 10.0f, false},
        {"Name", "Lamp", "name", 17, UIPropertyKind::String, 0.0f, 0.0f, false},
    };
    attach(entity, root, editor);

    UIRuntime runtime;
    runtime.set_root(root);
    runtime.set_viewport({0.0f, 0.0f, 640.0f, 480.0f});
    runtime.update(pointer_input(-1.0f, -1.0f));
    const UIRect* rect = runtime.rect_for(editor);
    CHECK_TRUE(rect != nullptr);
    if (!rect) return;
    const UIRect area = *rect;
    UIInputFrame select = pointer_input(area.x + 20.0f,
                                        area.y + editor->row_height * 1.5f);
    select.pointer_down[0] = true;
    select.pointer_pressed[0] = true;
    runtime.update(select);
    CHECK_INT(editor->selected, 1);

    UIInputFrame edit = pointer_input(area.x + 20.0f,
                                      area.y + editor->row_height * 1.5f);
    edit.pointer_down[0] = true;
    edit.key_down.push_back({static_cast<int>(gryce_engine::ui::UIKey::Enter), false});
    runtime.update(edit);
    CHECK_TRUE(editor->editing);
    CHECK_TRUE(editor->edit_buffer == "1.0");

    UIInputFrame clear = pointer_input(area.x + 20.0f,
                                       area.y + editor->row_height * 1.5f);
    clear.pointer_down[0] = true;
    clear.key_down.push_back({static_cast<int>(gryce_engine::ui::UIKey::Backspace), false});
    clear.key_down.push_back({static_cast<int>(gryce_engine::ui::UIKey::Backspace), false});
    clear.key_down.push_back({static_cast<int>(gryce_engine::ui::UIKey::Backspace), false});
    clear.characters = {'2', '.', '5'};
    runtime.update(clear);
    UIInputFrame commit = pointer_input(area.x + 20.0f,
                                        area.y + editor->row_height * 1.5f);
    commit.pointer_down[0] = true;
    commit.key_down.push_back({static_cast<int>(gryce_engine::ui::UIKey::Enter), false});
    runtime.update(commit);
    CHECK_TRUE(!editor->editing);
    CHECK_INT(editor->committed, 1);
    CHECK_TRUE(editor->rows[1].value == "2.5");
}

TEST(widgets, theme_tokens_and_animation_transitions) {
    gryce_engine::ui::set_ui_theme_preset(UIThemePreset::Dark);
    gryce_engine::ui::set_ui_theme_color(UIThemeColor::Panel, 0x112233FF);
    CHECK_TRUE(gryce_engine::ui::load_ui_theme_json(
        R"({"colors":{"panel":"#445566FF"},"metrics":{"row_height":40}})"));
    CHECK_INT(gryce_engine::ui::ui_theme_color(UIThemeColor::Panel), 0x445566FF);
    CHECK_NEAR(gryce_engine::ui::ui_theme_metric(
                   gryce_engine::ui::UIThemeMetric::RowHeight), 40.0f);
    gryce_engine::ui::set_ui_theme_color(UIThemeColor::Panel, 0x112233FF);
    gryce_engine::scene::Entity entity("ThemeMotion");
    auto* root = entity.add_component<UIPanel>();
    auto* button = entity.add_component<UIButton>();
    button->set_local_position(Vector3f(10.0f, 10.0f, 0.0f));
    button->hover_duration = 0.1f;
    button->press_duration = 0.1f;
    attach(entity, root, button);

    UIRuntime runtime;
    runtime.set_root(root);
    runtime.set_viewport({0.0f, 0.0f, 640.0f, 480.0f});
    UIInputFrame idle = pointer_input(-1.0f, -1.0f);
    idle.delta_time = 0.05f;
    runtime.update(idle);
    UIDrawList themed_draw;
    runtime.build_draw_list(themed_draw);
    CHECK_TRUE(!themed_draw.commands().empty());
    if (!themed_draw.commands().empty()) {
        CHECK_INT(themed_draw.commands().front().color, 0x112233FF);
    }

    UIInputFrame hover = pointer_input(20.0f, 20.0f);
    hover.delta_time = 0.05f;
    runtime.update(hover);
    hover.delta_time = 0.05f;
    runtime.update(hover);
    CHECK_TRUE(button->hover_amount() > 0.0f);
    CHECK_TRUE(button->hover_amount() < 1.0f);

    button->animate_opacity(0.25f, 0.1f);
    button->animate_offset(Vector2f(12.0f, 0.0f), 0.1f);
    UIInputFrame fade = hover;
    fade.pointer = Vector2f(-1.0f, -1.0f);
    fade.delta_time = 0.05f;
    runtime.update(fade);
    CHECK_TRUE(button->visual_opacity() > 0.25f);
    CHECK_TRUE(button->visual_opacity() < 1.0f);
    const UIRect* moved = runtime.rect_for(button);
    CHECK_TRUE(moved != nullptr);
    if (moved) CHECK_TRUE(moved->x > 10.0f);

    gryce_engine::ui::set_ui_theme_preset(UIThemePreset::Light);
    runtime.update(fade);
    UIDrawList light_draw;
    runtime.build_draw_list(light_draw);
    CHECK_TRUE(!light_draw.commands().empty());
    if (!light_draw.commands().empty()) {
        CHECK_INT(light_draw.commands().front().color,
                  gryce_engine::ui::ui_theme_color(UIThemeColor::Panel));
    }
    gryce_engine::ui::set_ui_theme_preset(UIThemePreset::Dark);
}

TEST(drawlist, round_rect_carries_radii_border_and_scales_alpha) {
    gryce_engine::ui::UIDrawList list;
    const gryce_engine::ui::UIRect rect{0.0f, 0.0f, 100.0f, 40.0f};
    const float radii[4] = {6.0f, 6.0f, 2.0f, 2.0f};
    list.add_round_rect(rect, radii, 0x112233FFu, 0x445566FFu, 1.0f, 0x8899AAFFu);
    CHECK_INT(list.size(), 1);
    if (list.size() != 1) return;
    const auto& command = list.commands().front();
    // 圆角仍然是 Rect 命令，只是多了半径与描边字段：后端据此决定走矩形还是
    // 圆角展开，模型层不需要新的命令类型。
    CHECK_TRUE(command.type == gryce_engine::ui::UIDrawCommandType::Rect);
    CHECK_NEAR(command.radii[0], 6.0f);
    CHECK_NEAR(command.radii[3], 2.0f);
    CHECK_INT(command.color_bottom, 0x445566FFu);
    CHECK_NEAR(command.border_width, 1.0f);
    CHECK_TRUE(command.rounded());

    gryce_engine::ui::UIDrawList faded;
    faded.append(list, 0.5f);
    CHECK_INT(faded.commands().front().color & 0xFFu, 128);
    // 描边与渐变底色必须一起淡出，否则整体透明时描边会残留成实线。
    CHECK_INT(faded.commands().front().border_color & 0xFFu, 128);
    CHECK_INT(faded.commands().front().color_bottom & 0xFFu, 128);

    gryce_engine::ui::UIDrawList shadow;
    shadow.add_round_shadow(rect, 8.0f, 12.0f, 0x000000AAu);
    CHECK_INT(shadow.size(), 1);
    if (shadow.size() == 1) {
        CHECK_TRUE(shadow.commands().front().type ==
                   gryce_engine::ui::UIDrawCommandType::Shadow);
        CHECK_NEAR(shadow.commands().front().spread, 12.0f);
        CHECK_NEAR(shadow.commands().front().radii[0], 8.0f);
    }
}

TEST(widgets, fluent_radius_tokens_and_panel_surface) {
    using gryce_engine::ui::UIThemeMetric;
    using gryce_engine::ui::UIThemePreset;
    using gryce_engine::ui::UIRadiusTier;
    gryce_engine::ui::set_ui_theme_preset(UIThemePreset::Fluent);
    // Fluent 的圆角阶梯：控件 4、卡片 8、浮层 12。
    CHECK_NEAR(gryce_engine::ui::ui_theme_radius(UIRadiusTier::Small), 4.0f);
    CHECK_NEAR(gryce_engine::ui::ui_theme_radius(UIRadiusTier::Medium), 8.0f);
    CHECK_NEAR(gryce_engine::ui::ui_theme_radius(UIRadiusTier::Large), 12.0f);
    CHECK_TRUE(gryce_engine::ui::ui_theme_metric(UIThemeMetric::Elevation) > 0.0f);

    // 半径阶梯随预设变化：高对比度收紧到 2/3/4 并关掉投影。
    gryce_engine::ui::set_ui_theme_preset(UIThemePreset::HighContrast);
    CHECK_NEAR(gryce_engine::ui::ui_theme_radius(UIRadiusTier::Small), 2.0f);
    CHECK_NEAR(gryce_engine::ui::ui_theme_radius(UIRadiusTier::Large), 4.0f);
    CHECK_NEAR(gryce_engine::ui::ui_theme_metric(UIThemeMetric::Elevation), 0.0f);

    gryce_engine::ui::set_ui_theme_preset(UIThemePreset::Dark);
    gryce_engine::scene::Entity entity("FluentSurface");
    auto* root = entity.add_component<UIPanel>();
    auto* card = entity.add_component<UIPanel>();
    card->size = Vector2f(200.0f, 80.0f);
    card->rest_elevation = 8.0f;
    attach(entity, root, card);

    UIRuntime runtime;
    runtime.set_root(root);
    runtime.set_viewport({0.0f, 0.0f, 400.0f, 200.0f});
    runtime.update(pointer_input(-1.0f, -1.0f));
    gryce_engine::ui::UIDrawList draw_list;
    runtime.build_draw_list(draw_list);

    int shadows = 0;
    float card_radius = -1.0f;
    for (const auto& command : draw_list.commands()) {
        if (command.type == gryce_engine::ui::UIDrawCommandType::Shadow) {
            ++shadows;
            // 投影在填充之前提交，否则会被自己的表面盖住。
            CHECK_TRUE(card_radius < 0.0f);
        }
        if (command.type == gryce_engine::ui::UIDrawCommandType::Rect &&
            std::fabs(command.rect.w - 200.0f) < 0.5f) {
            card_radius = command.radii[0];
        }
    }
    // 根面板不带层次（0 命令），卡片带一条投影 + 4px/8px 圆角。
    CHECK_INT(shadows, 1);
    CHECK_TRUE(card_radius > 0.0f);
}

TEST(widgets, info_bar_severity_accent_and_close_flow) {
    using gryce_engine::components::UIInfoBar;
    using gryce_engine::components::UIInfoBarSeverity;
    gryce_engine::ui::set_ui_theme_preset(UIThemePreset::Dark);

    gryce_engine::scene::Entity entity("InfoBar");
    auto* bar = entity.add_component<UIInfoBar>();
    bar->title = "Unreferenced asset";
    bar->message = "textures/rock.png is not used.";
    bar->severity = UIInfoBarSeverity::Warning;

    const UIRect rect{0.0f, 0.0f, 520.0f, 56.0f};
    gryce_engine::ui::UIDrawList list;
    bar->add_draw_commands(rect, list);

    // 严重度映射到强调色：强调条与左侧圆点都用对应语义色，不能一律取 Accent。
    const uint32_t warning = gryce_engine::ui::ui_theme_color(UIThemeColor::Warning);
    const uint32_t accent = gryce_engine::ui::ui_theme_color(UIThemeColor::Accent);
    CHECK_TRUE(warning != accent);
    int warning_hits = 0;
    int text_runs = 0;
    for (const auto& command : list.commands()) {
        if (command.type == gryce_engine::ui::UIDrawCommandType::Rect &&
            command.color == warning) {
            ++warning_hits;
        }
        if (command.type == gryce_engine::ui::UIDrawCommandType::Text) {
            ++text_runs;
            // 行内文本默认垂直居中，浮层/提示条不必逐处指定对齐方式。
            CHECK_TRUE(command.valign == gryce_engine::ui::UITextVAlign::Middle);
        }
    }
    CHECK_TRUE(warning_hits >= 2);
    CHECK_INT(text_runs, 2);

    // 关闭按钮的命中区与绘制矩形必须重合，否则点了没反应或误触别处。
    const UIRect close = bar->close_rect(rect);
    UIEvent move;
    move.type = UIEventType::PointerMove;
    move.layout_rect = rect;
    move.position = {close.x + 4.0f, close.y + 4.0f};
    CHECK_TRUE(bar->on_ui_event(move));
    CHECK_INT(bar->hover_close, 1);

    move.position = {rect.x + 10.0f, rect.y + 10.0f};
    CHECK_TRUE(!bar->on_ui_event(move));
    CHECK_INT(bar->hover_close, 0);

    UIEvent click;
    click.type = UIEventType::Click;
    click.layout_rect = rect;
    click.position = {close.x + close.w * 0.5f, close.y + close.h * 0.5f};
    CHECK_TRUE(bar->on_ui_event(click));
    CHECK_INT(bar->close_requested, 1);
    CHECK_TRUE(bar->dismissed);
    // 关掉之后整棵子树不再可见，宿主据此决定是否真的移除。
    CHECK_TRUE(!bar->is_visible_in_tree());

    // 不可关闭时点击不产生副作用。
    auto* locked = entity.add_component<UIInfoBar>();
    locked->closable = false;
    CHECK_TRUE(!locked->on_ui_event(click));
    CHECK_INT(locked->close_requested, 0);
    CHECK_TRUE(!locked->dismissed);
}

TEST(widgets, navigation_view_footer_rows_and_selection) {
    using gryce_engine::components::UINavigationView;
    gryce_engine::scene::Entity entity("NavigationView");
    auto* nav = entity.add_component<UINavigationView>();
    nav->header = "Workspace";
    nav->row_height = 40.0f;
    nav->items = {{"Home", "", false},
                  {"Scenes", "", false},
                  {"History", "", false},
                  {"Settings", "", true}};
    nav->selected = 0;

    const UIRect rect{0.0f, 0.0f, 240.0f, 400.0f};
    // 主列表从标题下方顺排；footer 项贴着容器下沿往上排，中间留白。
    CHECK_NEAR(nav->row_top(0, rect), 48.0f);
    CHECK_NEAR(nav->row_top(1, rect), 88.0f);
    CHECK_NEAR(nav->row_top(2, rect), 128.0f);
    CHECK_NEAR(nav->row_top(3, rect), 352.0f);
    CHECK_TRUE(nav->row_top(3, rect) + nav->row_height <= rect.bottom());

    CHECK_INT(nav->item_at(rect, 100.0f, 60.0f), 0);
    CHECK_INT(nav->item_at(rect, 100.0f, 100.0f), 1);
    CHECK_INT(nav->item_at(rect, 100.0f, 200.0f), -1);
    CHECK_INT(nav->item_at(rect, 100.0f, 360.0f), 3);

    UIEvent click;
    click.type = UIEventType::Click;
    click.layout_rect = rect;
    click.position = {100.0f, 360.0f};
    CHECK_TRUE(nav->on_ui_event(click));
    CHECK_INT(nav->selected, 3);
    CHECK_INT(nav->activated, 3);

    // 选中态与未选中态都要有可见反馈：选中行画淡底 + 左侧强调条。
    gryce_engine::ui::UIDrawList list;
    nav->add_draw_commands(rect, list);
    const uint32_t accent = gryce_engine::ui::ui_theme_color(UIThemeColor::Accent);
    int accent_bars = 0;
    for (const auto& command : list.commands()) {
        if (command.type == gryce_engine::ui::UIDrawCommandType::Rect &&
            command.color == accent && command.rect.w <= 4.0f) {
            ++accent_bars;
        }
    }
    CHECK_INT(accent_bars, 1);
}

static int g_texture_loads = 0;

bool fake_texture_loader(const char* path, gryce_engine::ui::TextureRef* out) {
    ++g_texture_loads;
    if (std::strcmp(path, "res:/ui/broken.png") == 0) return false;
    out->index = 7;
    out->generation = 3;
    out->width = 64;
    out->height = 32;
    return true;
}

TEST(widgets, texture_service_cache_and_fit_modes) {
    using gryce_engine::ui::TextureRef;
    gryce_engine::ui::ui_clear_texture_cache();
    gryce_engine::ui::ui_set_texture_loader(nullptr);
    // 没有宿主 loader 时退化为"无效贴图"，控件据此画占位色而不是崩。
    CHECK_TRUE(!gryce_engine::ui::ui_texture("res:/ui/a.png").valid());

    gryce_engine::ui::ui_set_texture_loader(&fake_texture_loader);
    g_texture_loads = 0;
    const TextureRef first = gryce_engine::ui::ui_texture("res:/ui/a.png");
    CHECK_TRUE(first.valid());
    CHECK_INT(first.index, 7);
    CHECK_INT(first.width, 64);
    CHECK_INT(g_texture_loads, 1);

    // 同一路径只解码上传一次。
    CHECK_TRUE(gryce_engine::ui::ui_texture("res:/ui/a.png").valid());
    CHECK_INT(g_texture_loads, 1);

    // 绘制期只查缓存：未命中的路径不能触发加载（加载回调会回取渲染器，
    // 与绘制期同一把锁，直接加载就是自锁）。
    CHECK_TRUE(!gryce_engine::ui::ui_texture_cached("res:/ui/never.png").valid());
    CHECK_INT(g_texture_loads, 1);

    // 失败也进缓存，避免每帧重试解码。
    CHECK_TRUE(!gryce_engine::ui::ui_texture("res:/ui/broken.png").valid());
    CHECK_INT(g_texture_loads, 2);
    CHECK_TRUE(!gryce_engine::ui::ui_texture("res:/ui/broken.png").valid());
    CHECK_INT(g_texture_loads, 2);

    // 换图（清缓存）后能重新加载到新的句柄。
    gryce_engine::ui::ui_clear_texture_cache();
    CHECK_TRUE(!gryce_engine::ui::ui_texture_cached("res:/ui/a.png").valid());
    CHECK_TRUE(gryce_engine::ui::ui_texture("res:/ui/a.png").valid());
    CHECK_INT(g_texture_loads, 3);
    gryce_engine::ui::ui_set_texture_loader(nullptr);
    gryce_engine::ui::ui_clear_texture_cache();

    // 填充模式：Contain 完整放下并居中，Cover 铺满并裁 UV，Stretch 变形填满。
    const UIRect box{0.0f, 0.0f, 200.0f, 140.0f};
    const gryce_engine::ui::UIFittedImage contain =
        gryce_engine::ui::ui_fit_image(box, 640, 360, gryce_engine::ui::UIFitMode::Contain);
    CHECK_NEAR(contain.rect.w, 200.0f);
    CHECK_NEAR(contain.rect.h, 112.5f);
    CHECK_NEAR(contain.rect.y, 13.75f);
    CHECK_NEAR(contain.u0, 0.0f);
    CHECK_NEAR(contain.u1, 1.0f);

    const gryce_engine::ui::UIFittedImage cover =
        gryce_engine::ui::ui_fit_image(box, 640, 360, gryce_engine::ui::UIFitMode::Cover);
    CHECK_NEAR(cover.rect.w, 200.0f);
    CHECK_NEAR(cover.rect.h, 140.0f);
    // 源更宽 -> 左右裁一半，UV 从 0 收到 1 之间。
    CHECK_TRUE(cover.u0 > 0.0f && cover.u1 < 1.0f);
    CHECK_NEAR(cover.u0 + cover.u1, 1.0f);

    const gryce_engine::ui::UIFittedImage stretch =
        gryce_engine::ui::ui_fit_image(box, 64, 64, gryce_engine::ui::UIFitMode::Stretch);
    CHECK_NEAR(stretch.rect.w, 200.0f);
    CHECK_NEAR(stretch.rect.h, 140.0f);
    CHECK_NEAR(stretch.u1, 1.0f);

    // 无尺寸信息时不猜，直接退化为盒子本身（调用方会画占位）。
    const gryce_engine::ui::UIFittedImage empty =
        gryce_engine::ui::ui_fit_image(box, 0, 0, gryce_engine::ui::UIFitMode::Contain);
    CHECK_NEAR(empty.rect.w, 200.0f);
    CHECK_NEAR(empty.rect.h, 140.0f);
}

TEST(drawlist, overlay_layer_is_split_out_of_page_commands) {
    using gryce_engine::components::UIMenuFlyout;
    gryce_engine::ui::set_ui_theme_preset(UIThemePreset::Dark);

    gryce_engine::scene::Entity entity("OverlayLayer");
    auto* root = entity.add_component<UIPanel>();
    root->size = Vector2f(400.0f, 300.0f);
    auto* label = entity.add_component<UILabel>();
    label->text = "page text";
    label->font_size = 14.0f;
    label->set_local_position(Vector3f(160.0f, 120.0f, 0.0f));
    label->size = Vector2f(200.0f, 24.0f);
    auto* flyout = entity.add_component<UIMenuFlyout>();
    flyout->set_entries({{"New Scene", "Ctrl+N", 1}, {"Open...", "Ctrl+O", 2}});
    flyout->size = Vector2f(200.0f, 96.0f);
    flyout->place_at(10.0f, 10.0f);
    flyout->set_open(false);
    attach(entity, root, label);
    attach(entity, root, flyout);

    UIRuntime runtime;
    runtime.set_root(root);
    runtime.set_viewport({0.0f, 0.0f, 400.0f, 300.0f});
    runtime.update(pointer_input(-1.0f, -1.0f));

    // 未打开时浮层整棵子树不参与绘制，也就没有置顶层。
    gryce_engine::ui::UIDrawList closed_list;
    runtime.build_draw_list(closed_list);
    CHECK_INT(static_cast<int>(closed_list.top_layer_start()), 0);

    // 打开需要显式让绘制缓存失效（宿主在界面结构变化时调用）。
    flyout->set_open(true);
    runtime.invalidate();
    runtime.update(pointer_input(-1.0f, -1.0f));
    gryce_engine::ui::UIDrawList list;
    runtime.build_draw_list(list);
    const int split = static_cast<int>(list.top_layer_start());
    CHECK_TRUE(split > 0);
    CHECK_TRUE(split < static_cast<int>(list.size()));

    // 分层点之前是页面（矩形 + 文字），之后才是浮层。渲染器在 split 处
    // 先提交前一层，否则浮层的实底会被页面的文字批次压住。
    bool page_text_seen = false;
    int overlay_rects = 0;
    for (int i = 0; i < static_cast<int>(list.size()); ++i) {
        const auto& command = list.commands()[static_cast<std::size_t>(i)];
        if (i < split && command.type == gryce_engine::ui::UIDrawCommandType::Text &&
            command.text == "page text") {
            page_text_seen = true;
        }
        if (i >= split && command.type == gryce_engine::ui::UIDrawCommandType::Rect) {
            ++overlay_rects;
        }
    }
    CHECK_TRUE(page_text_seen);
    CHECK_TRUE(overlay_rects > 0);
}

int main(int argc, char** argv) {
    std::vector<std::string> wanted;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--suite") == 0 && i + 1 < argc) {
            wanted.emplace_back(argv[++i]);
        }
    }

    int total = 0;
    int cases_failed = 0;
    for (const Case& item : registry()) {
        if (!wanted.empty()) {
            bool selected = false;
            for (const auto& suite : wanted) selected = selected || suite == item.suite;
            if (!selected) continue;
        }
        case_failed = 0;
        const int before = failed;
        item.fn();
        ++total;
        if (failed != before) {
            ++cases_failed;
            std::printf("  [FAIL] %s.%s\n", item.suite.c_str(), item.name.c_str());
        } else {
            std::printf("  [ OK ] %s.%s\n", item.suite.c_str(), item.name.c_str());
        }
    }

    std::printf("cases=%d failed=%d checks=%d\n", total, cases_failed, checks);
    return cases_failed == 0 ? 0 : 1;
}
