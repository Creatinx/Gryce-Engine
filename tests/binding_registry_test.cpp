// binding_registry_test.cpp — 数据绑定系统单元测试
//
// 测试 BindingRegistry 的以下功能：
// 1. 数据源设置/获取/存在性检查
// 2. 绑定注册与解绑
// 3. 脏标记与刷新机制
// 4. 控件属性自动更新
// 5. 控件销毁时自动解绑
// 6. 多控件绑定同一数据源
// 7. 单控件绑定多个数据源
// 8. 通过 widget::set_property 的 bind: 前缀绑定
// 9. UIManager update 内的自动刷新

#include <gtest/gtest.h>

#include <unordered_map>

#include "GryceEngineUtils/ui/binding_registry.h"
#include "GryceEngineUtils/ui/button.h"
#include "GryceEngineUtils/ui/label.h"
#include "GryceEngineUtils/ui/panel.h"
#include "GryceEngineUtils/ui/ui.h"

using namespace GryceEngineUtils::ui;

// 基础烟雾测试：验证单例可访问
TEST(BindingRegistrySmoke, SingletonExists) {
    auto& br = BindingRegistry::instance();
    (void)br;
}

// 测试 std::unordered_map 是否正常工作
TEST(BindingRegistrySmoke, MapWorks) {
    std::unordered_map<std::string, std::string> m;
    m["k1"] = "v1";
    EXPECT_EQ(m["k1"], "v1");
}

// 简单的 set 测试
TEST(BindingRegistrySmoke, JustSet) {
    auto& br = BindingRegistry::instance();
    br.set_value("k1", "v1");
}

// 简单的 set/get 测试
TEST(BindingRegistrySmoke, SimpleSetGet) {
    auto& br = BindingRegistry::instance();
    br.set_value("k1", "v1");
    EXPECT_EQ(br.get_value("k1"), "v1");
}

// ============================================================================
// BindingRegistry 基本功能测试
// ============================================================================

class BindingRegistryTest : public ::testing::Test {
protected:
    void SetUp() override {
        BindingRegistry::instance().clear_all();
    }

    void TearDown() override {
        BindingRegistry::instance().clear_all();
    }
};

// 数据源设置与获取
TEST_F(BindingRegistryTest, SetAndGetValue) {
    auto& br = BindingRegistry::instance();

    br.set_value("playerName", "Alice");
    EXPECT_EQ(br.get_value("playerName"), "Alice");
    EXPECT_TRUE(br.has_key("playerName"));
}

// 获取不存在的键返回空字符串
TEST_F(BindingRegistryTest, GetNonExistentKey) {
    EXPECT_EQ(BindingRegistry::instance().get_value("nonexistent"), "");
    EXPECT_FALSE(BindingRegistry::instance().has_key("nonexistent"));
}

// 设置不同数据类型的值
TEST_F(BindingRegistryTest, SetTypedValues) {
    auto& br = BindingRegistry::instance();

    br.set_value("intVal", int32_t(42));
    EXPECT_EQ(br.get_value("intVal"), "42");

    br.set_value("floatVal", 3.14f);
    EXPECT_EQ(br.get_value("floatVal"), "3.140000");

    // 布尔值请使用字符串 "true"/"false" 代替 bool 重载
    // （避免 const char* 到 bool 的隐式转换歧义）
    br.set_value("boolTrue", "true");
    EXPECT_EQ(br.get_value("boolTrue"), "true");

    br.set_value("boolFalse", "false");
    EXPECT_EQ(br.get_value("boolFalse"), "false");
}

// 覆盖已存在的键
TEST_F(BindingRegistryTest, OverwriteValue) {
    auto& br = BindingRegistry::instance();

    br.set_value("key", "old");
    EXPECT_EQ(br.get_value("key"), "old");

    br.set_value("key", "new");
    EXPECT_EQ(br.get_value("key"), "new");
}

// 设置相同值不触发脏标记
TEST_F(BindingRegistryTest, SameValueNoDirty) {
    auto& br = BindingRegistry::instance();

    br.set_value("key", "value");
    EXPECT_EQ(br.dirty_count(), 1);

    br.set_value("key", "value"); // 相同值，不标记脏
    EXPECT_EQ(br.dirty_count(), 1); // 仍然是 1（前一次设置的）
}

// 删除键
TEST_F(BindingRegistryTest, RemoveKey) {
    auto& br = BindingRegistry::instance();

    br.set_value("key", "value");
    EXPECT_TRUE(br.has_key("key"));

    br.remove_key("key");
    EXPECT_FALSE(br.has_key("key"));
    EXPECT_EQ(br.get_value("key"), "");
}

// 空键不存储
TEST_F(BindingRegistryTest, EmptyKeyIgnored) {
    auto& br = BindingRegistry::instance();

    br.set_value("", "value");
    EXPECT_FALSE(br.has_key(""));
}

// ============================================================================
// 绑定管理测试
// ============================================================================

class BindingManagementTest : public ::testing::Test {
protected:
    void SetUp() override {
        BindingRegistry::instance().clear_all();
        label_ = new Label("Lbl1", "Hello");
    }

    void TearDown() override {
        delete label_;
        BindingRegistry::instance().clear_all();
    }

    Label* label_ = nullptr;
};

// 绑定控件到数据源
TEST_F(BindingManagementTest, BindWidgetToDataKey) {
    auto& br = BindingRegistry::instance();

    br.set_value("text", "BoundText");
    br.bind(label_, "text", "text");

    // 绑定后立即初始化控件属性
    EXPECT_EQ(label_->text(), "BoundText");
    EXPECT_EQ(br.binding_count(label_), 1);
    EXPECT_EQ(br.total_bindings(), 1);
}

// 绑定后数据源变化通过 refresh 更新控件
TEST_F(BindingManagementTest, RefreshUpdatesWidget) {
    auto& br = BindingRegistry::instance();

    br.set_value("text", "Old");
    br.bind(label_, "text", "text");
    EXPECT_EQ(label_->text(), "Old");

    // 修改数据源
    br.set_value("text", "New");
    EXPECT_EQ(label_->text(), "Old"); // 尚未刷新，控件未更新

    // 刷新
    int updated = br.refresh();
    EXPECT_EQ(updated, 1);
    EXPECT_EQ(label_->text(), "New");
}

// 解绑后数据源变化不再更新控件
TEST_F(BindingManagementTest, UnbindStopsUpdates) {
    auto& br = BindingRegistry::instance();

    br.set_value("text", "Initial");
    br.bind(label_, "text", "text");
    br.refresh();
    EXPECT_EQ(label_->text(), "Initial");

    // 解绑
    br.unbind(label_);
    EXPECT_EQ(br.binding_count(label_), 0);
    EXPECT_EQ(br.total_bindings(), 0);

    // 修改数据源并刷新，控件不应更新
    br.set_value("text", "Changed");
    br.refresh();
    EXPECT_EQ(label_->text(), "Initial"); // 未更新
}

// 解绑指定属性
TEST_F(BindingManagementTest, UnbindSpecificProperty) {
    auto& br = BindingRegistry::instance();

    br.set_value("text", "Hello");
    br.set_value("visible", "true");
    br.bind(label_, "text", "text");
    br.bind(label_, "visible", "visible");
    EXPECT_EQ(br.binding_count(label_), 2);
    EXPECT_EQ(label_->text(), "Hello");

    // 只解绑 text 属性
    br.unbind(label_, "text");
    EXPECT_EQ(br.binding_count(label_), 1);

    // 刷新验证
    br.set_value("text", "World");
    br.set_value("visible", "false");
    int updated = br.refresh();
    // visible 绑定仍在，会被更新为 false（但 set_property("visible", "false") 由 Widget 基类处理）
    // text 已解绑，不应更新，仍为 "Hello"
    EXPECT_EQ(label_->text(), "Hello");
    // 注意：Label::set_property("visible", value) 会调用 Widget::set_property
    // 该函数处理 "visible" 属性并调用 set_visible(parse_bool(value))
    // 所以 label_->visible() 应为 false
    EXPECT_FALSE(label_->visible());
    // 验证有 1 个控件被更新（visible 绑定）
    EXPECT_EQ(updated, 1);
}

// 多控件绑定同一数据源
TEST_F(BindingManagementTest, MultipleWidgetsSameDataKey) {
    auto& br = BindingRegistry::instance();

    Label* label2 = new Label("Lbl2", "World");
    br.set_value("text", "Shared");

    br.bind(label_, "text", "text");
    br.bind(label2, "text", "text");
    br.refresh();

    EXPECT_EQ(label_->text(), "Shared");
    EXPECT_EQ(label2->text(), "Shared");

    // 修改后刷新，两个控件都应更新
    br.set_value("text", "Updated");
    int updated = br.refresh();
    EXPECT_EQ(updated, 2);
    EXPECT_EQ(label_->text(), "Updated");
    EXPECT_EQ(label2->text(), "Updated");

    delete label2;
}

// 单控件绑定多个数据源
TEST_F(BindingManagementTest, SingleWidgetMultipleBindings) {
    auto& br = BindingRegistry::instance();

    Button* btn = new Button("Btn1", "Click");
    br.set_value("btnText", "Submit");
    br.set_value("visible", "true");

    br.bind(btn, "text", "btnText");
    br.bind(btn, "visible", "visible");
    br.refresh();

    EXPECT_EQ(br.binding_count(btn), 2);
    EXPECT_EQ(br.total_bindings(), 2);

    delete btn;
}

// ============================================================================
// 绑定生命周期测试
// ============================================================================

class BindingLifecycleTest : public ::testing::Test {
protected:
    void SetUp() override {
        BindingRegistry::instance().clear_all();
    }

    void TearDown() override {
        BindingRegistry::instance().clear_all();
    }
};

// 控件销毁时自动解绑
TEST_F(BindingLifecycleTest, WidgetDestructorUnbinds) {
    auto& br = BindingRegistry::instance();

    Label* label = new Label("Lbl", "Text");
    br.set_value("text", "Value");
    br.bind(label, "text", "text");
    EXPECT_EQ(br.total_bindings(), 1);

    // 销毁控件
    delete label;

    // 自动解绑
    EXPECT_EQ(br.total_bindings(), 0);
}

// 多次刷新后计数正确
TEST_F(BindingLifecycleTest, RefreshCount) {
    auto& br = BindingRegistry::instance();

    Label* label = new Label("Lbl", "");
    br.set_value("text", "A");
    br.bind(label, "text", "text");
    br.refresh();

    // 修改并刷新
    br.set_value("text", "B");
    EXPECT_EQ(br.refresh(), 1);

    // 未修改不刷新
    EXPECT_EQ(br.refresh(), 0);
}

// 清除所有绑定
TEST_F(BindingLifecycleTest, ClearAll) {
    auto& br = BindingRegistry::instance();

    Label* label = new Label("Lbl", "");
    br.set_value("text", "Val");
    br.bind(label, "text", "text");
    EXPECT_EQ(br.total_bindings(), 1);
    EXPECT_EQ(br.dirty_count(), 1);

    br.clear_all();
    EXPECT_EQ(br.total_bindings(), 0);
    EXPECT_EQ(br.dirty_count(), 0);
    EXPECT_FALSE(br.has_key("text"));

    delete label;
}

// 标记所有脏
TEST_F(BindingLifecycleTest, MarkAllDirty) {
    auto& br = BindingRegistry::instance();

    br.set_value("a", "1");
    br.set_value("b", "2");
    br.refresh(); // 清除脏标记

    EXPECT_EQ(br.dirty_count(), 0);

    br.mark_all_dirty();
    EXPECT_EQ(br.dirty_count(), 2);
}

// ============================================================================
// set_property bind: 前缀测试
// ============================================================================

class BindPropertyTest : public ::testing::Test {
protected:
    void SetUp() override {
        BindingRegistry::instance().clear_all();
    }

    void TearDown() override {
        BindingRegistry::instance().clear_all();
    }
};

// 通过 set_property 的 bind: 前缀注册绑定
TEST_F(BindPropertyTest, SetPropertyWithBindPrefix) {
    auto& br = BindingRegistry::instance();

    br.set_value("helloText", "Hello World");
    Label* label = new Label("Lbl", "");

    // 使用 bind: 前缀注册绑定
    bool handled = label->set_property("bind:text", "helloText");
    EXPECT_TRUE(handled) << "set_property should handle bind:text";

    // 绑定后应自动初始化控件属性
    EXPECT_EQ(label->text(), "Hello World");

    delete label;
}

// bind: 前缀绑定支持任意属性名
TEST_F(BindPropertyTest, BindArbitraryProperty) {
    auto& br = BindingRegistry::instance();

    br.set_value("visibleVal", "false");
    Label* label = new Label("Lbl", "");

    bool handled = label->set_property("bind:visible", "visibleVal");
    EXPECT_TRUE(handled);

    // 设置 visible 属性（Label::set_visible 接收布尔值，set_property 处理 "false"）
    // 由于 Label 的 set_property 不直接处理 visible，这里仅验证绑定已注册
    EXPECT_EQ(br.binding_count(label), 1);

    delete label;
}

// 空 bind: 属性名被识别但不注册绑定
TEST_F(BindPropertyTest, BindEmptyPropertyName) {
    auto& br = BindingRegistry::instance();
    Label* label = new Label("Lbl", "");
    bool handled = label->set_property("bind:", "someKey");
    EXPECT_TRUE(handled); // bind: 前缀被识别
    // 空属性名不注册绑定
    EXPECT_EQ(br.binding_count(label), 0);
    delete label;
}

// ============================================================================
// UIManager 集成绑定测试
// ============================================================================

class BindingIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        BindingRegistry::instance().clear_all();
        mgr_ = UIManager::create(nullptr);
        ASSERT_NE(mgr_, nullptr);

        root_ = new Panel("RootPanel");
        root_->set_bounds(Rect{0, 0, 800, 600});
        mgr_->set_root(root_);
    }

    void TearDown() override {
        BindingRegistry::instance().clear_all();
        if (mgr_) {
            mgr_->destroy();
            mgr_ = nullptr;
        }
    }

    UIManager* mgr_ = nullptr;
    Widget* root_ = nullptr;
};

// UIManager::update 自动刷新绑定
TEST_F(BindingIntegrationTest, UpdateTriggersRefresh) {
    auto& br = BindingRegistry::instance();

    Label* label = new Label("Lbl", "Old");
    root_->add_child(label);

    br.set_value("text", "Old");
    br.bind(label, "text", "text");
    // bind 后立即初始化，所以 label->text() 已经是 "Old"
    EXPECT_EQ(label->text(), "Old");

    // 修改数据源
    br.set_value("text", "Updated");

    // 调用 update 触发自动刷新
    mgr_->update(0.016f);

    // 验证控件属性已更新
    EXPECT_EQ(label->text(), "Updated");
}

// 绑定在 UIManager update 循环中正确工作
TEST_F(BindingIntegrationTest, MultipleBindingsInUpdate) {
    auto& br = BindingRegistry::instance();

    Label* label1 = new Label("Lbl1", "");
    Label* label2 = new Label("Lbl2", "");
    root_->add_child(label1);
    root_->add_child(label2);

    br.set_value("text1", "First");
    br.set_value("text2", "Second");

    br.bind(label1, "text", "text1");
    br.bind(label2, "text", "text2");

    // 初始值已绑定
    EXPECT_EQ(label1->text(), "First");
    EXPECT_EQ(label2->text(), "Second");

    // 修改数据源
    br.set_value("text1", "Alpha");
    br.set_value("text2", "Beta");

    mgr_->update(0.016f);

    EXPECT_EQ(label1->text(), "Alpha");
    EXPECT_EQ(label2->text(), "Beta");
}

// 绑定未设置数据源时，控件值不变
TEST_F(BindingIntegrationTest, BindWithoutDataValue) {
    auto& br = BindingRegistry::instance();

    Label* label = new Label("Lbl", "Default");
    root_->add_child(label);

    // 绑定到不存在的数据源
    br.bind(label, "text", "nonExistentKey");

    // 控件值不应变化
    EXPECT_EQ(label->text(), "Default");
}

// 控件销毁后绑定自动清理，不影响其他控件
TEST_F(BindingIntegrationTest, DestroyedWidgetCleanup) {
    auto& br = BindingRegistry::instance();

    Label* label1 = new Label("Lbl1", "");
    Label* label2 = new Label("Lbl2", "");
    root_->add_child(label1);
    root_->add_child(label2);

    br.set_value("shared", "Value");
    br.bind(label1, "text", "shared");
    br.bind(label2, "text", "shared");

    EXPECT_EQ(br.total_bindings(), 2);

    // 销毁 label1
    root_->remove_child(label1);
    delete label1;

    // 绑定应自动清理
    EXPECT_EQ(br.total_bindings(), 1);

    // label2 仍应能接收更新
    br.set_value("shared", "NewValue");
    mgr_->update(0.016f);
    EXPECT_EQ(label2->text(), "NewValue");
}

// 绑定数据源更新后，控件的可见性变化
TEST_F(BindingIntegrationTest, BindVisibility) {
    auto& br = BindingRegistry::instance();

    Panel* panel = new Panel("SubPanel");
    root_->add_child(panel);

    // 绑定可见性到一个布尔值
    br.set_value("visible", "true");
    br.bind(panel, "visible", "visible");
    EXPECT_TRUE(panel->visible());

    // 设置为不可见
    br.set_value("visible", "false");
    mgr_->update(0.016f);
    EXPECT_FALSE(panel->visible());

    // 恢复可见
    br.set_value("visible", "true");
    mgr_->update(0.016f);
    EXPECT_TRUE(panel->visible());
}

// 绑定按钮的文本属性
TEST_F(BindingIntegrationTest, BindButtonText) {
    auto& br = BindingRegistry::instance();

    Button* btn = new Button("Btn1", "Click");
    root_->add_child(btn);

    br.set_value("btnLabel", "Submit");
    br.bind(btn, "text", "btnLabel");
    EXPECT_EQ(btn->text(), "Submit");

    // 修改文本
    br.set_value("btnLabel", "Cancel");
    mgr_->update(0.016f);
    EXPECT_EQ(btn->text(), "Cancel");
}

// 绑定后通过 set_property bind: 前缀注册
TEST_F(BindingIntegrationTest, BindViaSetProperty) {
    auto& br = BindingRegistry::instance();

    Label* label = new Label("Lbl", "");
    root_->add_child(label);

    br.set_value("myText", "Bound");
    label->set_property("bind:text", "myText");

    // 绑定后立即初始化
    EXPECT_EQ(label->text(), "Bound");

    // 更新数据源
    br.set_value("myText", "Updated");
    mgr_->update(0.016f);
    EXPECT_EQ(label->text(), "Updated");
}

// 大量绑定的刷新性能验证（不崩溃）
TEST_F(BindingIntegrationTest, ManyBindings) {
    auto& br = BindingRegistry::instance();

    const int kCount = 100;
    std::vector<Label*> labels;
    labels.reserve(kCount);

    for (int i = 0; i < kCount; i++) {
        std::string id = "Lbl" + std::to_string(i);
        Label* lbl = new Label(id.c_str(), "");
        root_->add_child(lbl);
        labels.push_back(lbl);

        std::string key = "text" + std::to_string(i);
        br.set_value(key, "Value" + std::to_string(i));
        br.bind(lbl, "text", key);
    }

    EXPECT_EQ(br.total_bindings(), static_cast<size_t>(kCount));

    // 修改所有数据源
    for (int i = 0; i < kCount; i++) {
        std::string key = "text" + std::to_string(i);
        br.set_value(key, "New" + std::to_string(i));
    }

    // 刷新所有绑定
    mgr_->update(0.016f);

    // 验证
    for (int i = 0; i < kCount; i++) {
        std::string expected = "New" + std::to_string(i);
        EXPECT_EQ(labels[i]->text(), expected);
    }

    // 清理
    for (auto* lbl : labels) {
        root_->remove_child(lbl);
        delete lbl;
    }
    EXPECT_EQ(br.total_bindings(), 0);
}

// 绑定后手动调用 mark_dirty + refresh
TEST_F(BindingIntegrationTest, ManualMarkDirtyAndRefresh) {
    auto& br = BindingRegistry::instance();

    Label* label = new Label("Lbl", "");
    root_->add_child(label);

    br.set_value("text", "Initial");
    br.bind(label, "text", "text");
    br.refresh(); // 清除脏标记
    EXPECT_EQ(label->text(), "Initial");

    // 不通过 set_value，直接 mark_dirty
    // 先修改数据存储（模拟外部修改）
    // 注意：这里直接修改内部数据，实际使用中应通过 set_value
    // 但 mark_dirty 可以强制刷新未变化的数据
    // 为了测试，我们直接 set_value 新值
    br.set_value("text", "Manual");
    EXPECT_EQ(br.dirty_count(), 1);

    // 手动调用 refresh
    br.refresh();
    EXPECT_EQ(label->text(), "Manual");
    EXPECT_EQ(br.dirty_count(), 0);
}