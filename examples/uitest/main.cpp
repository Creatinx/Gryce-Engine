// uitest — UI 模块自检样例
//
// 覆盖全部控件 + 交互逻辑（命中测试/信号/焦点/拖拽/样式表/动画/模态），
// 并带 --selftest 自动化模式：模拟输入、断言结果、打印 PASS/FAIL 汇总，
// 失败时以非零退出码结束（供 CI / 直接运行验证）。
//
// 用法：
//   uitest            手动交互窗口
//   uitest --selftest 自动断言后退出

#include <GryceEngineUtils/renderer.h>
#include <GryceEngineUtils/ui/ui.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include <GLFW/glfw3.h>

#include "render/render_context.h"
#include "render/render_pipeline.h"

using namespace GryceEngineUtils;

namespace {

int g_pass = 0;
int g_fail = 0;

void check(bool ok, const char* name) {
    if (ok) {
        ++g_pass;
        std::printf("[PASS] %s\n", name);
    } else {
        ++g_fail;
        std::printf("[FAIL] %s\n", name);
    }
}

// ---- 自动化断言（--selftest）---------------------------------------------
int run_selftest(Renderer* r) {
    auto* ui = ui::UIManager::create(r);
    r->set_ui_manager(ui);

    // 样式表
    auto* ss = new ui::StyleSheet();
    const bool parse_ok = ss->parse(R"(
        Button {
            background: #3a6ea5;
            background-hover: #4d84c2;
            background-pressed: #2b5178;
            color: #ffffff;
            border-radius: 4px;
        }
        #the_label { font-size: 20px; color: #ffcc00; }
    )");
    check(parse_ok, "StyleSheet::parse");
    ui->set_global_style(ss);

    // 根面板：Absolute 布局，控件位置可预测
    auto* root = new ui::Panel("root");
    root->set_layout(ui::LayoutType::Absolute);
    root->set_anchor(ui::Anchor::Stretch);
    root->set_background(ui::Color(0.07f, 0.08f, 0.10f, 0.72f)); // 半透明，模拟 UI-only 场景
    ui->set_root(root);

    // Label + #id 选择器
    auto* label = new ui::Label("the_label", "UITest");
    label->set_position(20.0f, 20.0f);
    label->set_size(200.0f, 30.0f);
    root->add_child(label);
    const ui::Style resolved = ss->resolve(label);
    check(resolved.font_size > 19.0f && resolved.font_size < 21.0f, "StyleSheet #id font-size");
    check(resolved.color.r > 0.99f && resolved.color.g < 0.9f, "StyleSheet #id color");

    // Button + 点击信号
    int pressed_count = 0;
    int clicked_count = 0;
    auto* btn = new ui::Button("btn", "Click");
    btn->set_position(20.0f, 70.0f);
    btn->set_size(120.0f, 40.0f);
    btn->on_pressed.connect([&]() { ++pressed_count; });
    btn->on_click.connect([&](ui::Widget*) { ++clicked_count; });
    root->add_child(btn);

    // CheckBox + 切换信号
    int toggle_count = 0;
    bool last_checked = false;
    auto* cb = new ui::CheckBox("cb", "Check me");
    cb->set_position(20.0f, 130.0f);
    cb->set_size(180.0f, 30.0f);
    cb->on_toggled.connect([&](bool checked) { ++toggle_count; last_checked = checked; });
    root->add_child(cb);

    // Slider + 值变化信号
    int slider_events = 0;
    auto* slider = new ui::Slider("slider");
    slider->set_position(20.0f, 180.0f);
    slider->set_size(260.0f, 24.0f);
    slider->set_range(0.0f, 100.0f);
    slider->set_value(50.0f);
    slider->on_value_changed.connect([&](float) { ++slider_events; });
    root->add_child(slider);

    // TextInput + 文本/提交
    int submit_count = 0;
    auto* input = new ui::TextInput("input");
    input->set_position(20.0f, 230.0f);
    input->set_size(240.0f, 34.0f);
    input->set_placeholder("type...");
    input->on_submit.connect([&]() { ++submit_count; });
    root->add_child(input);

    // ProgressBar
    auto* pb = new ui::ProgressBar("pb");
    pb->set_position(20.0f, 290.0f);
    pb->set_size(240.0f, 20.0f);
    root->add_child(pb);

    // ScrollView + 内容
    auto* scroll = new ui::ScrollView("scroll");
    scroll->set_position(320.0f, 70.0f);
    scroll->set_size(300.0f, 160.0f);
    auto* content = new ui::Panel("content");
    content->set_layout(ui::LayoutType::Vertical);
    content->set_spacing(4.0f);
    content->set_size(280.0f, 400.0f);
    for (int i = 0; i < 15; ++i) {
        char id[24], text[48];
        std::snprintf(id, sizeof(id), "item_%d", i);
        std::snprintf(text, sizeof(text), "Item #%02d", i);
        auto* item = new ui::Label(id, text);
        item->set_size(260.0f, 22.0f);
        content->add_child(item);
    }
    scroll->set_content(content);
    root->add_child(scroll);

    // Image（纹理存在时才生效）
    auto* img = new ui::Image("img");
    img->set_position(320.0f, 260.0f);
    img->set_size(64.0f, 64.0f);
    auto* tex = r->load_texture("res:/textures/test.bmp");
    if (tex) {
        img->set_texture(tex);
        check(true, "Image::set_texture (纹理加载成功)");
    } else {
        check(false, "Image::set_texture (缺少 textures/test.bmp)");
    }
    root->add_child(img);

    // ---- 新控件：ToolBar / MenuBar / ComboBox / RadioButton / SpinBox ----
    auto* toolbar = new ui::ToolBar("toolbar");
    toolbar->set_position(20.0f, 340.0f);
    toolbar->set_size(620.0f, 40.0f);
    toolbar->add_button("tb_new", "New");
    toolbar->add_divider();
    toolbar->add_button("tb_open", "Open");
    toolbar->add_button("tb_save", "Save");
    root->add_child(toolbar);

    int menu_activate = 0;
    auto* menubar = new ui::MenuBar("menubar");
    menubar->set_position(20.0f, 400.0f);
    menubar->set_size(400.0f, 30.0f);
    auto* file_menu = menubar->add_menu("file", "File");
    menubar->add_item(file_menu, "open_item", "Open...", [&]() { ++menu_activate; });
    menubar->add_separator(file_menu);
    menubar->add_item(file_menu, "quit_item", "Quit", [&]() { ++menu_activate; });
    auto* edit_menu = menubar->add_menu("edit", "Edit");
    menubar->add_item(edit_menu, "undo_item", "Undo", [&]() { ++menu_activate; });
    root->add_child(menubar);

    int combo_selected = -1;
    auto* combo = new ui::ComboBox("combo");
    combo->set_position(240.0f, 450.0f); // 避开 radio/spin，防止弹出层被遮挡
    combo->set_size(200.0f, 30.0f);
    combo->add_item("Low");
    combo->add_item("Medium");
    combo->add_item("High");
    combo->on_selected.connect([&](int idx) { combo_selected = idx; });
    root->add_child(combo);

    auto* radio_a = new ui::RadioButton("radio_a", "Option A");
    radio_a->set_position(20.0f, 500.0f);
    radio_a->set_size(160.0f, 24.0f);
    radio_a->set_group("quality");
    root->add_child(radio_a);
    auto* radio_b = new ui::RadioButton("radio_b", "Option B");
    radio_b->set_position(20.0f, 530.0f);
    radio_b->set_size(160.0f, 24.0f);
    radio_b->set_group("quality");
    root->add_child(radio_b);

    auto* spin = new ui::SpinBox("spin");
    spin->set_position(20.0f, 570.0f);
    spin->set_size(180.0f, 34.0f);
    spin->set_range(-10.0f, 10.0f);
    spin->set_step(0.5f);
    spin->set_value(2.0f);
    root->add_child(spin);

    // ---- 断言：基础控件 ----
    check(label->text() == std::string("UITest"), "Label 文本");
    label->set_text("Hello");
    check(label->text() == std::string("Hello"), "Label set_text");

    pb->set_progress(1.5f);
    check(pb->progress() == 1.0f, "ProgressBar clamp 上限");
    pb->set_progress(-0.5f);
    check(pb->progress() == 0.0f, "ProgressBar clamp 下限");
    pb->set_progress(0.6f);

    // 先渲染一帧让布局/命中区域生效
    ui->render();

    const auto click_at = [&](float x, float y) {
        ui->on_mouse_move(x, y);
        ui->on_mouse_button(0, true);
        ui->on_mouse_button(0, false);
    };

    // ---- 断言：WidgetFactory ----
    check(ui::WidgetFactory::instance().is_registered("Button"), "WidgetFactory 内置 Button");
    check(ui::WidgetFactory::instance().is_registered("MenuBar"), "WidgetFactory 内置 MenuBar");
    auto* factory_btn = ui::WidgetFactory::instance().create("Button");
    check(factory_btn != nullptr && std::strcmp(factory_btn->type_name(), "Button") == 0,
          "WidgetFactory::create(Button)");
    check(ui::WidgetFactory::instance().create("NoSuchType") == nullptr,
          "WidgetFactory 未注册类型返回 nullptr");
    ui::WidgetFactory::instance().register_type("MyWidget",
        []() { return static_cast<ui::Widget*>(new ui::Label("my", "custom")); });
    auto* custom = ui::WidgetFactory::instance().create("MyWidget");
    check(custom != nullptr && std::strcmp(custom->type_name(), "Label") == 0,
          "WidgetFactory 自定义类型");
    delete custom;
    delete factory_btn;

    // ---- 断言：ToolBar ----
    check(toolbar->children().size() == 4, "ToolBar 按钮+分隔线数量");

    // ---- 断言：MenuBar ----
    menubar->open_menu(file_menu);
    check(menubar->is_open(), "MenuBar 展开");
    // 点击弹出菜单的第一项（Open...）
    ui::Widget* popup = nullptr;
    for (auto* child : menubar->children()) {
        if (std::strcmp(child->type_name(), "Panel") == 0) popup = child;
    }
    check(popup != nullptr, "MenuBar 弹出面板存在");
    if (popup && !popup->children().empty()) {
        const ui::Rect& item_b = popup->children().front()->bounds();
        click_at(item_b.x + item_b.w * 0.5f, item_b.y + item_b.h * 0.5f);
        check(menu_activate == 1, "MenuBar 菜单项触发");
        check(!menubar->is_open(), "MenuBar 选择后关闭");
    }

    // ---- 断言：ComboBox ----
    click_at(combo->bounds().x + combo->bounds().w * 0.5f, combo->bounds().y + combo->bounds().h * 0.5f);
    check(combo->is_open(), "ComboBox 展开");
    ui::Widget* combo_popup = nullptr;
    for (auto* child : combo->children()) {
        if (std::strcmp(child->type_name(), "Panel") == 0) combo_popup = child;
    }
    if (combo_popup && !combo_popup->children().empty()) {
        const ui::Rect& item_b = combo_popup->children()[1]->bounds();
        click_at(item_b.x + item_b.w * 0.5f, item_b.y + item_b.h * 0.5f);
        check(combo_selected == 1 && std::strcmp(combo->selected_text().c_str(), "Medium") == 0,
              "ComboBox 选中 Medium");
        check(!combo->is_open(), "ComboBox 选择后关闭");
    }

    // ---- 断言：RadioButton 互斥 ----
    click_at(radio_a->bounds().x + 10.0f, radio_a->bounds().y + 10.0f);
    check(radio_a->is_checked() && !radio_b->is_checked(), "RadioButton A 选中");
    click_at(radio_b->bounds().x + 10.0f, radio_b->bounds().y + 10.0f);
    check(radio_b->is_checked() && !radio_a->is_checked(), "RadioButton 互斥切换");

    // ---- 断言：SpinBox ----
    ui->update(1.0f / 60.0f); // 定位上下按钮
    const ui::Rect& up_b = spin->children()[0]->bounds();
    click_at(up_b.x + up_b.w * 0.5f, up_b.y + up_b.h * 0.5f);
    check(spin->value() > 2.49f && spin->value() < 2.51f, "SpinBox 上按钮 +0.5");
    const ui::Rect& down_b = spin->children()[1]->bounds();
    click_at(down_b.x + down_b.w * 0.5f, down_b.y + down_b.h * 0.5f);
    check(spin->value() > 1.99f && spin->value() < 2.01f, "SpinBox 下按钮 -0.5");

    // ---- 断言：按钮点击 ----
    click_at(btn->bounds().x + 20, btn->bounds().y + 20);
    check(pressed_count == 1, "Button on_pressed");
    check(clicked_count == 1, "Button on_click");
    check(btn->is_hovered(), "Button hover 状态");

    // ---- 断言：CheckBox ----
    click_at(cb->bounds().x + 10, cb->bounds().y + 10);
    check(toggle_count == 1 && last_checked, "CheckBox 首次点击选中");
    check(cb->is_checked(), "CheckBox is_checked");
    click_at(cb->bounds().x + 10, cb->bounds().y + 10);
    check(toggle_count == 2 && !last_checked, "CheckBox 再次点击取消");

    // ---- 断言：Slider 拖拽 ----
    const float slider_mid_x = slider->bounds().x + slider->bounds().w * 0.5f;
    const float slider_y = slider->bounds().y + slider->bounds().h * 0.5f;
    ui->on_mouse_move(slider_mid_x, slider_y);
    ui->on_mouse_button(0, true);
    ui->on_mouse_move(slider->bounds().x + slider->bounds().w * 0.75f, slider_y);
    ui->on_mouse_button(0, false);
    check(slider->normalized() > 0.7f && slider->normalized() < 0.8f,
          "Slider 拖拽到 75% 位置");
    check(slider_events >= 2, "Slider on_value_changed 触发");

    // ---- 断言：TextInput ----
    click_at(input->bounds().x + 20, input->bounds().y + 20);
    check(ui->focused_widget() == input, "TextInput 点击获得焦点");
    ui->on_text_input("abc");
    check(std::strcmp(input->text(), "abc") == 0, "TextInput 输入文本");
    ui->on_keyboard(259, true); // Backspace
    check(std::strcmp(input->text(), "ab") == 0, "TextInput Backspace");
    ui->on_keyboard(257, true); // Enter
    check(submit_count == 1, "TextInput Enter 提交");
    ui->set_focus(nullptr);

    // ---- 断言：ScrollView ----
    scroll->scroll_to(0.0f, 120.0f);
    check(scroll->scroll_y() == 120.0f, "ScrollView scroll_to");
    scroll->start_drag(0.0f, 200.0f);
    scroll->drag_to(0.0f, 180.0f);
    scroll->end_drag();
    check(scroll->scroll_y() == 140.0f, "ScrollView 拖拽滚动");

    // ---- 断言：动画 ----
    auto* fade = ui::Animation::fade_in(btn, 0.5f);
    fade->play();
    ui->update(0.25f);
    const float mid_opacity = btn->opacity();
    check(mid_opacity > 0.0f && mid_opacity < 1.0f, "Animation fade_in 中间透明度");
    ui->update(0.5f);
    check(btn->opacity() > 0.99f, "Animation fade_in 完成");

    auto* slide = ui::Animation::slide_in(root, 0.4f, ui::Direction::FromBottom);
    slide->play();
    ui->update(0.2f);
    check(root->position_y() > 0.0f, "Animation slide_in 中间位置");
    ui->update(0.5f);
    check(root->position_y() == 0.0f, "Animation slide_in 回到原位");

    // ---- 断言：模态 ----
    auto* modal = new ui::Panel("modal");
    modal->set_layout(ui::LayoutType::Absolute);
    ui->push_modal(modal);
    ui->render(); // 布局模态为全屏后命中测试才有效
    check(ui->hit_test_top(640.0f, 360.0f) == modal, "Modal 优先命中");
    ui->pop_modal();
    check(ui->hit_test_top(640.0f, 360.0f) != modal, "Modal 弹出后不再命中");

    // 残影/闪烁检测：UI-only 场景（无 3D 提交）下连续渲染 6 帧，
    // 鼠标在 A/B 两个控件间来回移动；每帧在 present 前截图（渲染线程内）
    // 输出到 ui_frames/frame_N.bmp，由外部脚本对比。
    const char* frame_dir = "D:/Gryce-Engine/build/utils-check/ui_frames";
    std::filesystem::create_directories(frame_dir);
    const float hover_pos[6][2] = {
        {80.0f, 90.0f}, {80.0f, 145.0f},
        {80.0f, 90.0f}, {80.0f, 145.0f},
        {80.0f, 90.0f}, {80.0f, 145.0f},
    };
    for (int i = 0; i < 6; ++i) {
        ui->on_mouse_move(hover_pos[i][0], hover_pos[i][1]);
        // 不用 begin_frame：它会把真实鼠标位置转发给 UI，覆盖模拟的 hover
        ui->update(1.0f / 60.0f);
        // 手动帧控制：clear（UI-only）→ UI → 截图 → present，
        // 确保截图读到本帧真实画面（Renderer::end_frame 内部无法插入截图）
        if (r->pipeline()->has_submitted_items()) {
            r->pipeline()->render_submitted(*r->context());
        } else {
            r->context()->clear(0.0f, 0.0f, 0.0f, 1.0f);
        }
        ui->render();
        char path[256];
        std::snprintf(path, sizeof(path), "%s/frame_%d.bmp", frame_dir, i);
        r->context()->push_command([path](gryce_engine::render::IRenderBackend* b) {
            b->capture_frame_to_file(path);
        });
        r->context()->present();
    }
    std::printf("[DIAG] 帧截图已写入 %s\n", frame_dir);

    std::printf("\nuitest selftest: %d passed, %d failed\n", g_pass, g_fail);
    ui->destroy();
    return g_fail == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char* argv[]) {
    const bool selftest = argc > 1 && std::strcmp(argv[1], "--selftest") == 0;

    auto* r = Renderer::create({
        .title = "Gryce UITest",
        .width = 1280,
        .height = 720,
        .api = RenderAPI::OpenGL,
    });
    if (!r) {
        std::fprintf(stderr, "uitest: Renderer::create failed\n");
        return 1;
    }

    // 诊断：窗口逻辑/物理尺寸与管线视口的一致性
    {
        int w = 0, h = 0;
        r->window_size(w, h);
        int fw = 0, fh = 0;
        std::printf("[DIAG] window logical=%dx%d", w, h);
#ifdef _WIN32
        // 直接查询 GLFW framebuffer 尺寸
        if (GLFWwindow* native = static_cast<GLFWwindow*>(r->native_window())) {
            glfwGetFramebufferSize(native, &fw, &fh);
        }
#endif
        std::printf(" framebuffer=%dx%d pipeline_viewport=%dx%d\n",
                    fw, fh,
                    r->pipeline() ? r->pipeline()->viewport_width() : 0,
                    r->pipeline() ? r->pipeline()->viewport_height() : 0);
    }

    if (selftest) {
        const int code = run_selftest(r);
        r->destroy();
        return code;
    }

    // ---- 手动交互模式 ----
    auto* ui = ui::UIManager::create(r);
    r->set_ui_manager(ui);

    auto* ss = new ui::StyleSheet();
    ss->parse(R"(
        Panel { background: rgba(16, 18, 24, 0.75); border-radius: 8px; }
        Button {
            background: #3a6ea5; background-hover: #4d84c2;
            background-pressed: #2b5178; color: #fff;
            border-radius: 4px; padding: 8px 16px; font-size: 15px;
        }
        Label { color: #ccc; font-size: 15px; }
        #title { font-size: 26px; color: #fff; }
        TextInput {
            background: #0f1115; background-hover: #15181e;
            border: 1px solid #3a6ea5; border-radius: 4px;
            padding: 6px 10px; font-size: 15px;
        }
    )");
    ui->set_global_style(ss);

    auto* root = new ui::Panel("root");
    root->set_layout(ui::LayoutType::Vertical);
    root->set_spacing(10.0f);
    root->set_padding({24.0f, 24.0f, 24.0f, 24.0f});
    root->set_anchor(ui::Anchor::Stretch);

    auto* title = new ui::Label("title", "Gryce UITest (manual)");
    title->set_style_id("title");
    root->add_child(title);

    auto* row = new ui::Panel("row");
    row->set_layout(ui::LayoutType::Horizontal);
    row->set_spacing(8.0f);
    auto* b1 = new ui::Button("b1", "Button A");
    b1->set_size(120.0f, 0.0f);
    b1->on_pressed.connect([]() { std::printf("Button A pressed\n"); });
    auto* b2 = new ui::Button("b2", "Button B");
    b2->set_size(120.0f, 0.0f);
    b2->on_pressed.connect([]() { std::printf("Button B pressed\n"); });
    row->add_child(b1);
    row->add_child(b2);
    root->add_child(row);

    auto* slider = new ui::Slider("volume");
    slider->set_size(320.0f, 0.0f);
    slider->set_range(0.0f, 100.0f);
    slider->set_value(50.0f);
    auto* value = new ui::Label("val", "Volume: 50.00");
    slider->on_value_changed.connect([value](float v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Volume: %.2f", v * 100.0f);
        value->set_text(buf);
    });
    root->add_child(slider);
    root->add_child(value);

    auto* cb = new ui::CheckBox("cb", "Enable feature");
    cb->on_toggled.connect([](bool on) { std::printf("Feature: %s\n", on ? "ON" : "OFF"); });
    root->add_child(cb);

    auto* input = new ui::TextInput("input");
    input->set_size(320.0f, 0.0f);
    input->set_placeholder("type here...");
    input->on_submit.connect([input]() { std::printf("Submit: %s\n", input->text()); });
    root->add_child(input);

    auto* pb = new ui::ProgressBar("pb");
    pb->set_size(320.0f, 0.0f);
    root->add_child(pb);

    auto* scroll = new ui::ScrollView("scroll");
    scroll->set_size(420.0f, 140.0f);
    auto* content = new ui::Panel("content");
    content->set_layout(ui::LayoutType::Vertical);
    content->set_spacing(4.0f);
    content->set_size(400.0f, 340.0f);
    for (int i = 0; i < 14; ++i) {
        char id[24], text[48];
        std::snprintf(id, sizeof(id), "it_%d", i);
        std::snprintf(text, sizeof(text), "Scroll item #%02d", i);
        auto* item = new ui::Label(id, text);
        item->set_size(380.0f, 22.0f);
        content->add_child(item);
    }
    scroll->set_content(content);
    root->add_child(scroll);

    // 工具栏 + 菜单栏 + 下拉框 + 单选 + 数值微调
    auto* toolbar = new ui::ToolBar("toolbar");
    toolbar->set_size(620.0f, 40.0f);
    toolbar->add_button("tb_new", "New")->on_pressed.connect([]() { std::printf("Toolbar: New\n"); });
    toolbar->add_divider();
    toolbar->add_button("tb_open", "Open")->on_pressed.connect([]() { std::printf("Toolbar: Open\n"); });
    root->add_child(toolbar);

    auto* menubar = new ui::MenuBar("menubar");
    menubar->set_size(420.0f, 30.0f);
    auto* file_menu = menubar->add_menu("file", "File");
    menubar->add_item(file_menu, "m_new", "New Project", []() { std::printf("Menu: New Project\n"); });
    menubar->add_separator(file_menu);
    menubar->add_item(file_menu, "m_quit", "Quit", []() { std::printf("Menu: Quit\n"); });
    auto* view_menu = menubar->add_menu("view", "View");
    menubar->add_item(view_menu, "m_grid", "Toggle Grid", []() { std::printf("Menu: Grid\n"); });
    root->add_child(menubar);

    auto* combo = new ui::ComboBox("combo");
    combo->set_size(200.0f, 30.0f);
    combo->add_item("Low");
    combo->add_item("Medium");
    combo->add_item("High");
    combo->on_text_selected.connect([](const char* s) { std::printf("Combo: %s\n", s); });
    root->add_child(combo);

    auto* radio_a = new ui::RadioButton("ra", "Quality: Fast");
    radio_a->set_group("quality");
    radio_a->set_checked(true);
    auto* radio_b = new ui::RadioButton("rb", "Quality: Best");
    radio_b->set_group("quality");
    root->add_child(radio_a);
    root->add_child(radio_b);

    auto* spin = new ui::SpinBox("spin");
    spin->set_size(180.0f, 34.0f);
    spin->set_range(0.0f, 10.0f);
    spin->set_step(0.5f);
    spin->set_value(3.0f);
    spin->on_value_changed.connect([](float v) { std::printf("Spin: %.2f\n", v); });
    root->add_child(spin);

    ui->set_root(root);
    ui::Animation::slide_in(root, 0.5f, ui::Direction::FromBottom)->play();
    ui::Animation::fade_in(title, 0.6f)->play();

    float t = 0.0f;
    while (r->is_running()) {
        r->begin_frame();
        t += static_cast<float>(r->delta_time());
        pb->set_progress(std::fmod(t, 1.0f));
        ui->update(static_cast<float>(r->delta_time()));
        r->end_frame();
    }

    ui->destroy();
    r->destroy();
    return 0;
}
