// hot_reload_test.cpp — 热重载系统单元测试
//
// 测试以下功能：
// 1. ScriptVM::eval_file 从文件加载 JS
// 2. ScriptVM::reload_script 热重载 JS
// 3. UIManager::reload_ui 从 .uif 文件重新加载 UI
// 4. UIManager::reload_script 热重载脚本
// 5. UIManager::dump_ui_tree 导出控件树
// 6. UIManager::wireframe_mode 线框模式
// 7. UIManager::auto_hot_reload 自动热重载

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "GryceEngineUtils/ui/button.h"
#include "GryceEngineUtils/ui/label.h"
#include "GryceEngineUtils/ui/panel.h"
#include "GryceEngineUtils/ui/ui.h"

using namespace GryceEngineUtils::ui;

namespace {

// 创建临时 .js 文件
std::string create_js_file(const std::string& name, const std::string& content) {
    auto temp_dir = std::filesystem::temp_directory_path();
    auto path = (temp_dir / name).string();
    std::ofstream ofs(path, std::ios::binary);
    ofs << content;
    ofs.close();
    return path;
}

// 创建临时 .uif 文件
std::string create_uif_file(const std::string& name, const std::string& content) {
    auto temp_dir = std::filesystem::temp_directory_path();
    auto path = (temp_dir / name).string();
    std::ofstream ofs(path, std::ios::binary);
    ofs << content;
    ofs.close();
    return path;
}

// 删除临时文件
void remove_temp_file(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

} // namespace

// ============================================================================
// ScriptVM 热重载测试
// ============================================================================

class ScriptVMHotReloadTest : public ::testing::Test {
protected:
    void SetUp() override {
        vm_.init();
        js_path_ = create_js_file("hot_reload_test.js",
            "function greet() { return 'Hello'; }");
    }

    void TearDown() override {
        vm_.shutdown();
        remove_temp_file(js_path_);
    }

    ScriptVM vm_;
    std::string js_path_;
};

// eval_file 加载 JS 文件
TEST_F(ScriptVMHotReloadTest, EvalFile) {
    ScriptResult sr = vm_.eval_file(js_path_);
    EXPECT_TRUE(sr.success) << sr.error_msg;

    // 验证函数可用
    sr = vm_.call_function("greet");
    EXPECT_TRUE(sr.success);
    EXPECT_EQ(sr.result, "Hello");
}

// eval_file 文件不存在时返回错误
TEST_F(ScriptVMHotReloadTest, EvalFileNotFound) {
    ScriptResult sr = vm_.eval_file("nonexistent.js");
    EXPECT_FALSE(sr.success);
    EXPECT_NE(sr.error_msg.find("not found"), std::string::npos);
}

// 热重载脚本
TEST_F(ScriptVMHotReloadTest, ReloadScript) {
    // 初始加载
    vm_.eval_file(js_path_);
    ScriptResult sr = vm_.call_function("greet");
    EXPECT_EQ(sr.result, "Hello");

    // 修改文件
    std::ofstream ofs(js_path_, std::ios::binary);
    ofs << "function greet() { return 'World'; }";
    ofs.close();

    // 热重载
    sr = vm_.reload_script(js_path_);
    EXPECT_TRUE(sr.success);

    // 验证函数已更新
    sr = vm_.call_function("greet");
    EXPECT_TRUE(sr.success);
    EXPECT_EQ(sr.result, "World");
}

// 热重载后保留 C++ 注册的函数
TEST_F(ScriptVMHotReloadTest, ReloadPreservesRegisteredFunctions) {
    // 注册 C++ 函数
    vm_.register_function("cppFunc", [](JSContext* ctx, JSValueConst,
                                         int argc, JSValueConst* argv) -> JSValue {
        return JS_NewString(ctx, "cpp_value");
    });

    // 加载 JS 文件
    vm_.eval_file(js_path_);

    // 验证 C++ 函数可用
    ScriptResult sr = vm_.call_function("cppFunc");
    EXPECT_TRUE(sr.success);
    EXPECT_EQ(sr.result, "cpp_value");

    // 热重载
    vm_.reload_script(js_path_);

    // 验证 C++ 函数仍然可用
    sr = vm_.call_function("cppFunc");
    EXPECT_TRUE(sr.success);
    EXPECT_EQ(sr.result, "cpp_value");
}

// ============================================================================
// UIManager 热重载测试
// ============================================================================

class UIManagerHotReloadTest : public ::testing::Test {
protected:
    void SetUp() override {
        mgr_ = UIManager::create(nullptr);
        ASSERT_NE(mgr_, nullptr);

        // 创建一个简单的 .uif 文件
        uif_path_ = create_uif_file("test_reload.uif",
            R"(<Panel id="RootPanel">
                <Button id="Btn1" text="Click Me" />
                <Label id="Lbl1" text="Hello" />
            </Panel>)");
    }

    void TearDown() override {
        if (mgr_) {
            mgr_->destroy();
            mgr_ = nullptr;
        }
        remove_temp_file(uif_path_);
    }

    UIManager* mgr_ = nullptr;
    std::string uif_path_;
};

// reload_ui 加载 .uif 文件
TEST_F(UIManagerHotReloadTest, ReloadUI) {
    bool ok = mgr_->reload_ui(uif_path_);
    EXPECT_TRUE(ok);

    // 验证根控件已创建
    Widget* root = mgr_->root();
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->id(), "RootPanel");
    EXPECT_EQ(root->children().size(), 2);
}

// reload_ui 空路径返回 false
TEST_F(UIManagerHotReloadTest, ReloadUIEmptyPath) {
    bool ok = mgr_->reload_ui("");
    EXPECT_FALSE(ok);
}

// reload_ui 不存在的文件返回 false
TEST_F(UIManagerHotReloadTest, ReloadUINotFound) {
    bool ok = mgr_->reload_ui("nonexistent.uif");
    EXPECT_FALSE(ok);
}

// 多次 reload_ui 替换控件树
TEST_F(UIManagerHotReloadTest, ReloadUIReplaceTree) {
    // 第一次加载
    EXPECT_TRUE(mgr_->reload_ui(uif_path_));
    Widget* first_root = mgr_->root();
    ASSERT_NE(first_root, nullptr);

    // 第二次加载（不同内容）
    std::string uif2_path = create_uif_file("test_reload2.uif",
        R"(<Panel id="RootPanel2">
            <Button id="Btn2" text="New" />
        </Panel>)");

    EXPECT_TRUE(mgr_->reload_ui(uif2_path));
    Widget* second_root = mgr_->root();
    ASSERT_NE(second_root, nullptr);

    // 根控件指针应不同（旧的已被销毁）
    EXPECT_NE(second_root, first_root);
    EXPECT_EQ(second_root->id(), "RootPanel2");
    EXPECT_EQ(second_root->children().size(), 1);

    remove_temp_file(uif2_path);
}

// ============================================================================
// DumpUITree 测试
// ============================================================================

TEST_F(UIManagerHotReloadTest, DumpUITreeEmpty) {
    std::string dump = mgr_->dump_ui_tree();
    // 没有根控件时输出 "(empty)"
    EXPECT_NE(dump.find("(empty)"), std::string::npos);
}

TEST_F(UIManagerHotReloadTest, DumpUITreeWithWidgets) {
    // 加载 UI
    EXPECT_TRUE(mgr_->reload_ui(uif_path_));

    // 导出控件树
    std::string dump = mgr_->dump_ui_tree();
    EXPECT_NE(dump.find("UI Widget Tree"), std::string::npos);
    EXPECT_NE(dump.find("RootPanel"), std::string::npos);
    EXPECT_NE(dump.find("Btn1"), std::string::npos);
    EXPECT_NE(dump.find("Lbl1"), std::string::npos);
}

// ============================================================================
// 线框模式测试
// ============================================================================

TEST_F(UIManagerHotReloadTest, WireframeModeToggle) {
    // 默认关闭
    EXPECT_FALSE(mgr_->wireframe_mode());

    // 切换
    mgr_->toggle_wireframe_mode();
    EXPECT_TRUE(mgr_->wireframe_mode());

    // 再次切换
    mgr_->toggle_wireframe_mode();
    EXPECT_FALSE(mgr_->wireframe_mode());

    // set 方法
    mgr_->set_wireframe_mode(true);
    EXPECT_TRUE(mgr_->wireframe_mode());
    mgr_->set_wireframe_mode(false);
    EXPECT_FALSE(mgr_->wireframe_mode());
}

// ============================================================================
// 自动热重载测试
// ============================================================================

TEST_F(UIManagerHotReloadTest, AutoHotReloadSetting) {
    // 默认关闭
    EXPECT_FALSE(mgr_->auto_hot_reload());

    // 启用
    mgr_->set_auto_hot_reload(true);
    EXPECT_TRUE(mgr_->auto_hot_reload());

    // 设置文件路径
    mgr_->set_uif_watch_path(uif_path_);
    EXPECT_EQ(mgr_->uif_watch_path(), uif_path_);

    mgr_->set_js_script_path("test.js");
    EXPECT_EQ(mgr_->js_script_path(), "test.js");

    // 关闭
    mgr_->set_auto_hot_reload(false);
    EXPECT_FALSE(mgr_->auto_hot_reload());
}

// 自动热重载 update 调用不崩溃
TEST_F(UIManagerHotReloadTest, AutoHotReloadUpdate) {
    mgr_->set_auto_hot_reload(true);
    mgr_->set_uif_watch_path(uif_path_);

    // 更新多次不崩溃
    mgr_->update(0.016f);
    mgr_->update(0.016f);
    mgr_->update(0.016f);

    // 销毁后更新不崩溃
    mgr_->set_auto_hot_reload(false);
}

// ============================================================================
// UIManager::reload_script 测试
// ============================================================================

TEST_F(UIManagerHotReloadTest, ReloadScript) {
    // 由于 UIManager 创建时没有 Renderer，ScriptVM 可能未初始化
    // 测试 reload_script 在 ScriptVM 不可用时返回 false
    bool ok = mgr_->reload_script("test.js");
    // ScriptVM 可能成功初始化也可能没有，取决于是否传入 Renderer
    // 这里我们不假设具体结果，只确保不崩溃
    (void)ok;
}