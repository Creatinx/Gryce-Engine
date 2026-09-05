// ui_demo — 完整 UI 演示（裸框架 + UI，场景 B）
//
// 覆盖：Widget 树 + 样式表 + 信号 + 动画 + 滚动容器，
// 以及 UI 覆盖在 3D 场景之上的合成；输入由 Renderer 自动转发
// （r->set_ui_manager(ui) 后无需手动调用 on_mouse_* / on_keyboard）。

#include <GryceEngineUtils/renderer.h>
#include <GryceEngineUtils/ui/ui.h>

#include <cmath>
#include <cstdio>

using namespace GryceEngineUtils;

int main() {
    auto* r = Renderer::create({
        .title = "Gryce UI Demo",
        .width = 1280,
        .height = 720,
        .api = RenderAPI::OpenGL,
    });
    if (!r) return 1;

    auto* ui = ui::UIManager::create(r);
    r->set_ui_manager(ui); // end_frame 自动渲染 UI + begin_frame 自动转发输入

    // ---- 样式表（CSS 子集）----
    auto* ss = new ui::StyleSheet();
    ss->parse(R"(
        Panel { background: rgba(18, 20, 26, 0.72); border-radius: 8px; }
        Button {
            background: #3a6ea5;
            background-hover: #4d84c2;
            background-pressed: #2b5178;
            color: #ffffff;
            border-radius: 4px;
            padding: 8px 16px;
            font-size: 15px;
        }
        Label { color: #cccccc; font-size: 15px; }
        #title { font-size: 26px; color: #ffffff; }
        TextInput {
            background: #0f1115;
            background-hover: #15181e;
            border: 1px solid #3a6ea5;
            border-radius: 4px;
            padding: 6px 10px;
            font-size: 15px;
        }
        CheckBox { font-size: 15px; }
    )");
    ui->set_global_style(ss);

    // ---- Widget 树 ----
    auto* root = new ui::Panel("root");
    root->set_layout(ui::LayoutType::Vertical);
    root->set_spacing(10.0f);
    root->set_padding({24.0f, 24.0f, 24.0f, 24.0f});
    root->set_anchor(ui::Anchor::Stretch);

    auto* title = new ui::Label("title", "Gryce Engine UI Demo");
    title->set_style_id("title");
    root->add_child(title);

    // 按钮行
    auto* row = new ui::Panel("row");
    row->set_layout(ui::LayoutType::Horizontal);
    row->set_spacing(8.0f);

    auto* play_btn = new ui::Button("play_btn", "PLAY");
    play_btn->set_size(110.0f, 0.0f);
    play_btn->on_pressed.connect([]() { std::printf("Play clicked!\n"); });
    row->add_child(play_btn);

    auto* quit_btn = new ui::Button("quit_btn", "QUIT");
    quit_btn->set_size(110.0f, 0.0f);
    quit_btn->on_pressed.connect([]() { std::printf("Quit clicked!\n"); });
    row->add_child(quit_btn);
    root->add_child(row);

    // 滑块 + 值显示
    auto* slider = new ui::Slider("volume");
    slider->set_size(320.0f, 0.0f);
    slider->set_range(0.0f, 100.0f);
    slider->set_value(50.0f);
    auto* slider_label = new ui::Label("volume_value", "Volume: 50.00");
    slider_label->set_size(220.0f, 0.0f);
    slider->on_value_changed.connect([slider_label](float v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Volume: %.2f", v * 100.0f);
        slider_label->set_text(buf);
    });
    root->add_child(slider);
    root->add_child(slider_label);

    // 复选框
    auto* vsync_cb = new ui::CheckBox("vsync_cb", "Enable V-Sync");
    vsync_cb->set_checked(true);
    vsync_cb->on_toggled.connect([](bool checked) {
        std::printf("V-Sync: %s\n", checked ? "ON" : "OFF");
    });
    root->add_child(vsync_cb);

    // 文本输入
    auto* input = new ui::TextInput("name_input");
    input->set_size(320.0f, 0.0f);
    input->set_placeholder("Type your name...");
    input->set_max_length(24);
    auto* input_label = new ui::Label("input_label", "Name: (empty)");
    input->on_text_changed.connect([input_label](const char* text) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "Name: %s", text);
        input_label->set_text(buf);
    });
    input->on_submit.connect([input]() { std::printf("Submit: %s\n", input->text()); });
    root->add_child(input);
    root->add_child(input_label);

    // 进度条（每帧模拟）
    auto* progress = new ui::ProgressBar("progress");
    progress->set_size(320.0f, 0.0f);
    root->add_child(progress);

    // 滚动容器：一列 Label 演示裁剪与滚动条
    auto* scroll = new ui::ScrollView("scroll");
    scroll->set_size(420.0f, 160.0f);
    auto* scroll_content = new ui::Panel("scroll_content");
    scroll_content->set_layout(ui::LayoutType::Vertical);
    scroll_content->set_spacing(4.0f);
    scroll_content->set_size(400.0f, 516.0f); // 20 × 22 + 19 × 4
    for (int i = 0; i < 20; ++i) {
        char id[32], text[64];
        std::snprintf(id, sizeof(id), "item_%d", i);
        std::snprintf(text, sizeof(text), "Scroll item #%02d", i);
        auto* item = new ui::Label(id, text);
        item->set_size(380.0f, 22.0f);
        scroll_content->add_child(item);
    }
    scroll->set_content(scroll_content);
    root->add_child(scroll);

    ui->set_root(root);

    // 入口动画
    ui::Animation::slide_in(root, 0.5f, ui::Direction::FromBottom)->play();
    ui::Animation::fade_in(title, 0.6f)->play();

    // ---- 3D 背景（一个旋转三角形）----
    const float verts[] = {
         0.0f,  1.2f, 0.0f,   0.0f, 0.0f, 1.0f,   0.5f, 0.0f,
        -1.4f, -1.0f, 0.0f,   0.0f, 0.0f, 1.0f,   0.0f, 1.0f,
         1.4f, -1.0f, 0.0f,   0.0f, 0.0f, 1.0f,   1.0f, 1.0f,
    };
    const uint32_t indices[] = {0, 1, 2};
    auto* mesh = r->create_mesh(verts, 3, indices, 3);
    auto* mat = r->create_material();
    mat->set_albedo({0.25f, 0.5f, 0.9f});

    LightData light;
    light.type = render::LightType::Directional;
    light.direction = math::Vector3f(-0.5f, -1.0f, -0.3f);
    light.color = math::Vector3f::one();
    light.intensity = 1.0f;
    r->set_lights(&light, 1);

    const math::Vector3f eye(0.0f, 2.0f, 5.0f);
    const math::Matrix4f proj = math::Matrix4f::perspective(
        math::to_radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
    r->set_camera(eye, proj * math::Matrix4f::look_at(
        eye, math::Vector3f::zero(), math::Vector3f::up()));

    float t = 0.0f;
    while (r->is_running()) {
        r->begin_frame();
        const float dt = static_cast<float>(r->delta_time());
        t += dt;

        // 3D
        r->draw(mesh, mat, math::Matrix4f::rotate(t, math::Vector3f::up()));

        // UI 更新（渲染由 end_frame 自动完成）
        progress->set_progress(std::fmod(t, 1.0f));
        ui->update(dt);

        r->end_frame();
    }

    ui->destroy(); // 释放 root 树 / 模态 / 样式表 / 动画
    r->destroy();
    return 0;
}
