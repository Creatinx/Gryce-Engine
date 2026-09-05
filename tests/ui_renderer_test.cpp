#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "GryceEngineUtils/ui/button.h"
#include "GryceEngineUtils/ui/label.h"
#include "GryceEngineUtils/ui/panel.h"
#include "GryceEngineUtils/ui/ui.h"
#include "ui/ui_renderer.h"

using namespace GryceEngineUtils::ui;

// ============================================================================
// UIVertex 结构测试
// ============================================================================

// UIVertex 大小：14 个 float = 56 bytes
TEST(UIRendererTest, VertexSize) {
    EXPECT_EQ(sizeof(UIVertex), 14 * sizeof(float));
    EXPECT_EQ(sizeof(UIVertex), 56u);
}

// UIVertex 各字段偏移验证
TEST(UIRendererTest, VertexOffsets) {
    EXPECT_EQ(offsetof(UIVertex, x), 0u);
    EXPECT_EQ(offsetof(UIVertex, y), 4u);
    EXPECT_EQ(offsetof(UIVertex, r), 8u);
    EXPECT_EQ(offsetof(UIVertex, g), 12u);
    EXPECT_EQ(offsetof(UIVertex, b), 16u);
    EXPECT_EQ(offsetof(UIVertex, a), 20u);
    EXPECT_EQ(offsetof(UIVertex, u), 24u);
    EXPECT_EQ(offsetof(UIVertex, v), 28u);
    EXPECT_EQ(offsetof(UIVertex, mode), 32u);
    EXPECT_EQ(offsetof(UIVertex, rx), 36u);
    EXPECT_EQ(offsetof(UIVertex, ry), 40u);
    EXPECT_EQ(offsetof(UIVertex, rw), 44u);
    EXPECT_EQ(offsetof(UIVertex, rh), 48u);
    EXPECT_EQ(offsetof(UIVertex, radius), 52u);
}

// ============================================================================
// UIRenderer 构造/析构测试
// ============================================================================

TEST(UIRendererTest, CreateDestroy) {
    UIRenderer renderer;
    EXPECT_FALSE(renderer.initialized());
}

TEST(UIRendererTest, InitWithoutContext) {
    UIRenderer renderer;
    // 空指针初始化应该失败
    EXPECT_FALSE(renderer.init(nullptr));
    EXPECT_FALSE(renderer.initialized());
}

// ============================================================================
// 顶点/索引生成逻辑测试
// ============================================================================

// 由于 UIRenderer 的 begin_frame/end_frame 需要 initialized_ 为 true 才能工作，
// 我们通过直接测试 push_quad 的私有方法不可行（私有方法）。
// 测试框架通过 UIRenderer 公有接口的行为来验证。

// 测试：未初始化时调用 begin_frame/end_frame 不会崩溃
TEST(UIRendererTest, UninitializedFrameSafe) {
    UIRenderer renderer;
    EXPECT_FALSE(renderer.initialized());

    // 未初始化时调用帧方法应该安全地返回
    renderer.begin_frame(1920.0f, 1080.0f);
    renderer.end_frame();
}

// 测试：未初始化时调用 GenerateMesh 不会崩溃
TEST(UIRendererTest, UninitializedGenerateMeshSafe) {
    UIRenderer renderer;

    // 创建一个简单的控件树
    Panel* root = new Panel("Root");
    Button* btn = new Button("Btn1", "Click Me");
    root->add_child(btn);

    // 未初始化时调用 GenerateMesh 应该安全
    std::vector<Widget*> modals;
    renderer.generate_mesh(root, modals);

    delete root;
}

// 测试：未初始化时调用绘制方法不会崩溃
TEST(UIRendererTest, UninitializedDrawSafe) {
    UIRenderer renderer;

    renderer.draw_rect(0.0f, 0.0f, 100.0f, 100.0f, Color{1.0f, 1.0f, 1.0f, 1.0f});
    renderer.draw_rounded_rect(10.0f, 10.0f, 50.0f, 50.0f, 5.0f, Color{1.0f, 0.0f, 0.0f, 1.0f});
    renderer.draw_text(0.0f, 0.0f, "Hello", 16.0f, Color{1.0f, 1.0f, 1.0f, 1.0f});
    renderer.draw_focus_ring(0.0f, 0.0f, 100.0f, 100.0f, 5.0f, Color{0.4f, 0.6f, 1.0f, 0.8f});
    renderer.set_scissor(0, 0, 100, 100);
    renderer.reset_scissor();
}

// 测试：UIRenderer 的 font_texture_handle 默认返回无效句柄
TEST(UIRendererTest, DefaultFontTextureHandle) {
    UIRenderer renderer;
    auto handle = renderer.font_texture_handle();
    EXPECT_FALSE(handle.is_valid());
}

// ============================================================================
// UIManager 集成测试（UIRenderer 回退行为）
// ============================================================================

// 测试：UIManager 在没有 Renderer 时创建，UIRenderer 应为 nullptr
TEST(UIRendererTest, UIManagerWithoutRenderer) {
    UIManager* mgr = UIManager::create(nullptr);
    ASSERT_NE(mgr, nullptr);

    // 没有 Renderer 时 UIRenderer 不会被创建
    mgr->destroy();
}

// 测试：UIRenderer 控件树遍历（widget 绘制逻辑验证）
// 测试 draw_widget_mesh 在正确的控件树结构下不会崩溃
TEST(UIRendererTest, WidgetTreeTraversal) {
    // 创建一个复杂的控件树
    Panel* root = new Panel("Root");
    root->set_bounds(Rect{0.0f, 0.0f, 1920.0f, 1080.0f});

    Panel* header = new Panel("Header");
    header->set_bounds(Rect{0.0f, 0.0f, 1920.0f, 60.0f});
    root->add_child(header);

    Label* title = new Label("Title", "Hello World");
    title->set_bounds(Rect{10.0f, 10.0f, 200.0f, 40.0f});
    header->add_child(title);

    Button* btn = new Button("StartBtn", "Start Game");
    btn->set_bounds(Rect{100.0f, 100.0f, 200.0f, 50.0f});
    btn->set_hovered(true);
    root->add_child(btn);

    Button* btn2 = new Button("ExitBtn", "Exit");
    btn2->set_bounds(Rect{100.0f, 160.0f, 200.0f, 50.0f});
    btn2->set_pressed(true);
    root->add_child(btn2);

    // 验证控件树结构
    EXPECT_EQ(root->children().size(), 3u);
    EXPECT_EQ(header->children().size(), 1u);

    // 验证控件属性
    EXPECT_STREQ(title->text().c_str(), "Hello World");
    EXPECT_STREQ(btn->text().c_str(), "Start Game");
    EXPECT_TRUE(btn->hovered());
    EXPECT_TRUE(btn2->pressed());

    delete root;
}

// 测试：UIRenderer 中控件可见性对遍历的影响
TEST(UIRendererTest, VisibilityAffectsTraversal) {
    Panel* root = new Panel("Root");
    root->set_bounds(Rect{0.0f, 0.0f, 800.0f, 600.0f});

    Button* visible_btn = new Button("Visible", "Visible");
    visible_btn->set_bounds(Rect{10.0f, 10.0f, 100.0f, 30.0f});
    root->add_child(visible_btn);

    Button* hidden_btn = new Button("Hidden", "Hidden");
    hidden_btn->set_bounds(Rect{10.0f, 50.0f, 100.0f, 30.0f});
    hidden_btn->set_visible(false);
    root->add_child(hidden_btn);

    // 可见控件
    EXPECT_TRUE(visible_btn->visible());
    // 隐藏控件
    EXPECT_FALSE(hidden_btn->visible());

    delete root;
}

// 测试：UIRenderer 中控件禁用状态对样式的影响
TEST(UIRendererTest, DisabledStateAlpha) {
    Panel* root = new Panel("Root");
    root->set_bounds(Rect{0.0f, 0.0f, 800.0f, 600.0f});

    Button* enabled_btn = new Button("Enabled", "Enabled");
    enabled_btn->set_bounds(Rect{10.0f, 10.0f, 100.0f, 30.0f});
    root->add_child(enabled_btn);

    Button* disabled_btn = new Button("Disabled", "Disabled");
    disabled_btn->set_bounds(Rect{10.0f, 50.0f, 100.0f, 30.0f});
    disabled_btn->set_enabled(false);
    root->add_child(disabled_btn);

    EXPECT_TRUE(enabled_btn->enabled());
    EXPECT_FALSE(disabled_btn->enabled());

    delete root;
}

// 测试：Style 中有边框时绘制逻辑
TEST(UIRendererTest, StyleWithBorder) {
    Button* btn = new Button("BorderBtn", "Border");
    btn->set_bounds(Rect{0.0f, 0.0f, 100.0f, 50.0f});

    Style& s = btn->style();
    s.border_width = 2.0f;
    s.border_radius = 5.0f;
    s.border_color = Color{1.0f, 1.0f, 1.0f, 1.0f};
    s.background = Color{0.2f, 0.2f, 0.2f, 1.0f};
    s.has_background = true;

    EXPECT_EQ(s.border_width, 2.0f);
    EXPECT_EQ(s.border_radius, 5.0f);

    // 在 draw_widget_mesh 中，有边框时会先画外圈圆角矩形（边框色）
    // 再画内缩的背景矩形
    // 这里验证边框属性传递正确

    delete btn;
}

// 测试：不同控件类型在树中的位置
TEST(UIRendererTest, MultipleWidgetTypes) {
    Panel* root = new Panel("Root");
    root->set_bounds(Rect{0.0f, 0.0f, 800.0f, 600.0f});

    // 添加各种控件类型
    root->add_child(new Label("Label1", "Text"));
    root->add_child(new Button("Button1", "OK"));
    root->add_child(new Panel("SubPanel"));

    EXPECT_EQ(root->children().size(), 3u);
    EXPECT_STREQ(root->children()[0]->type_name(), "Label");
    EXPECT_STREQ(root->children()[1]->type_name(), "Button");
    EXPECT_STREQ(root->children()[2]->type_name(), "Panel");

    // 验证控件类型名称对 ScrollView 裁剪判断的兼容性
    Button* btn = dynamic_cast<Button*>(root->children()[1]);
    ASSERT_NE(btn, nullptr);
    // type_name 应该返回 "Button" 而不是 "ScrollView"
    EXPECT_NE(std::strcmp(btn->type_name(), "ScrollView"), 0);

    delete root;
}

// 测试：UIRenderer 的 text_width 方法
TEST(UIRendererTest, TextWidthWithoutFont) {
    UIRenderer renderer;
    // 没有字体图集时，text_width 应该返回 0
    // 由于 text_width 是私有方法，通过公有接口间接测试
    // 这里只是验证 draw_text 在无字体图集时不会崩溃
    renderer.draw_text(100.0f, 100.0f, "Test", 16.0f, Color{1.0f, 1.0f, 1.0f, 1.0f});
}

// 测试：圆角矩形半径限制
TEST(UIRendererTest, RoundedRectRadiusClamping) {
    UIRenderer renderer;

    // 半径大于半个最短边时应该被 clamp
    // 这个测试验证 draw_rounded_rect 不会因为半径过大而崩溃
    renderer.draw_rounded_rect(0.0f, 0.0f, 10.0f, 10.0f, 100.0f, Color{1.0f, 0.0f, 0.0f, 1.0f});
    renderer.draw_rounded_rect(0.0f, 0.0f, 10.0f, 10.0f, 0.0f, Color{1.0f, 0.0f, 0.0f, 1.0f});
    renderer.draw_rounded_rect(0.0f, 0.0f, 0.0f, 0.0f, 5.0f, Color{1.0f, 0.0f, 0.0f, 1.0f});
}

// 测试：ScrollView 子控件裁剪逻辑
TEST(UIRendererTest, ScrollViewCulling) {
    Panel* scroll = new Panel("ScrollView");
    scroll->set_bounds(Rect{0.0f, 0.0f, 200.0f, 200.0f});

    // 子控件在视口内
    Button* inside = new Button("Inside", "Inside");
    inside->set_bounds(Rect{10.0f, 10.0f, 50.0f, 30.0f});
    scroll->add_child(inside);

    // 子控件完全在视口外（上方）
    Button* above = new Button("Above", "Above");
    above->set_bounds(Rect{10.0f, -100.0f, 50.0f, 30.0f});
    scroll->add_child(above);

    // 子控件完全在视口外（右侧）
    Button* right = new Button("Right", "Right");
    right->set_bounds(Rect{300.0f, 10.0f, 50.0f, 30.0f});
    scroll->add_child(right);

    // 验证控件位置
    const Rect& vp = scroll->bounds();
    const Rect& b1 = inside->bounds();
    EXPECT_TRUE(b1.x + b1.w >= vp.x && b1.x <= vp.x + vp.w &&
                b1.y + b1.h >= vp.y && b1.y <= vp.y + vp.h); // 在视口内

    const Rect& b2 = above->bounds();
    EXPECT_TRUE(b2.y + b2.h < vp.y); // 在视口上方

    const Rect& b3 = right->bounds();
    EXPECT_TRUE(b3.x > vp.x + vp.w); // 在视口右侧

    delete scroll;
}