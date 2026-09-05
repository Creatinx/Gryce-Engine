#include <gtest/gtest.h>

#include <string>

#include "GryceEngineUtils/ui/button.h"
#include "GryceEngineUtils/ui/label.h"
#include "GryceEngineUtils/ui/panel.h"
#include "GryceEngineUtils/ui/ui.h"
#include "ui/engine_bridge.h"
#include "ui/script_vm.h"

using namespace GryceEngineUtils::ui;

// ============================================================================
// EngineBridge 测试
// ============================================================================

class EngineBridgeTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 直接创建 ScriptVM 并初始化 EngineBridge
        vm_ = new ScriptVM();
        ASSERT_TRUE(vm_->init());

        // 手动注册 engine.*（不依赖 UIManager）
        EngineBridge::init(vm_, nullptr);
    }

    void TearDown() override {
        if (vm_) {
            vm_->shutdown();
            delete vm_;
            vm_ = nullptr;
        }
    }

    ScriptVM* vm_ = nullptr;
};

// 验证 engine 对象存在
TEST_F(EngineBridgeTest, EngineObjectExists) {
    ScriptResult sr = vm_->eval("typeof engine");
    EXPECT_TRUE(sr.success);
    EXPECT_EQ(sr.result, "object");
}

// 验证 engine.loadScene 是函数
TEST_F(EngineBridgeTest, LoadSceneIsFunction) {
    ScriptResult sr = vm_->eval("typeof engine.loadScene");
    EXPECT_TRUE(sr.success);
    EXPECT_EQ(sr.result, "function");
}

// 调用 engine.loadScene（不实际加载，只是验证不崩溃）
TEST_F(EngineBridgeTest, CallLoadScene) {
    ScriptResult sr = vm_->eval("engine.loadScene('res:/scenes/test.gesc')");
    EXPECT_TRUE(sr.success);
}

// 验证 engine.playSound 是函数
TEST_F(EngineBridgeTest, PlaySoundIsFunction) {
    ScriptResult sr = vm_->eval("typeof engine.playSound");
    EXPECT_TRUE(sr.success);
    EXPECT_EQ(sr.result, "function");
}

// 调用 engine.playSound（不崩溃）
TEST_F(EngineBridgeTest, CallPlaySound) {
    ScriptResult sr = vm_->eval("engine.playSound('test.wav')");
    EXPECT_TRUE(sr.success);
}

// 验证 engine.showDialog 是函数
TEST_F(EngineBridgeTest, ShowDialogIsFunction) {
    ScriptResult sr = vm_->eval("typeof engine.showDialog");
    EXPECT_TRUE(sr.success);
    EXPECT_EQ(sr.result, "function");
}

// 调用 engine.showDialog 无 UIManager（应抛出错误）
TEST_F(EngineBridgeTest, CallShowDialogNoManager) {
    ScriptResult sr = vm_->eval(
        "try { engine.showDialog('test'); 'ok' } catch(e) { 'error:' + e.message }");
    EXPECT_TRUE(sr.success);
    EXPECT_NE(sr.result.find("error:"), std::string::npos)
        << "Should throw error w/o UIManager, got: " << sr.result;
}

// 验证 engine.closeDialog 是函数
TEST_F(EngineBridgeTest, CloseDialogIsFunction) {
    ScriptResult sr = vm_->eval("typeof engine.closeDialog");
    EXPECT_TRUE(sr.success);
    EXPECT_EQ(sr.result, "function");
}

// 调用 engine.closeDialog 无 UIManager（应抛出错误）
TEST_F(EngineBridgeTest, CallCloseDialog) {
    ScriptResult sr = vm_->eval(
        "try { engine.closeDialog(); 'ok' } catch(e) { 'error:' + e.message }");
    EXPECT_TRUE(sr.success);
    EXPECT_NE(sr.result.find("error:"), std::string::npos)
        << "Should throw error w/o UIManager, got: " << sr.result;
}

// 验证 engine.getString 是函数
TEST_F(EngineBridgeTest, GetStringIsFunction) {
    ScriptResult sr = vm_->eval("typeof engine.getString");
    EXPECT_TRUE(sr.success);
    EXPECT_EQ(sr.result, "function");
}

// 调用 engine.getString
TEST_F(EngineBridgeTest, CallGetString) {
    ScriptResult sr = vm_->eval("engine.getString('menu.start')");
    EXPECT_TRUE(sr.success);
    EXPECT_EQ(sr.result, "menu.start");
}

// 验证 engine.bind 是函数
TEST_F(EngineBridgeTest, BindIsFunction) {
    ScriptResult sr = vm_->eval("typeof engine.bind");
    EXPECT_TRUE(sr.success);
    EXPECT_EQ(sr.result, "function");
}

// 调用 engine.bind
TEST_F(EngineBridgeTest, CallBind) {
    ScriptResult sr = vm_->eval("engine.bind('test', function() {})");
    EXPECT_TRUE(sr.success);
}

// 验证 engine 对象的所有方法
TEST_F(EngineBridgeTest, AllMethodsExist) {
    ScriptResult sr = vm_->eval(
        "var methods = ['loadScene', 'playSound', 'showDialog', 'closeDialog', 'getString', 'bind'];"
        "methods.every(function(m) { return typeof engine[m] === 'function'; })");
    EXPECT_TRUE(sr.success);
    EXPECT_EQ(sr.result, "true");
}

// ============================================================================
// 事件回调集成测试
// ============================================================================

// 测试 Widget 的 event_callbacks_ 机制
TEST(EventBridgeTest, SetEventCallback) {
    Button btn("Btn1", "Click Me");
    btn.set_event_callback("click", "onButtonClick");
    btn.set_event_callback("focus", "onButtonFocus");
    btn.set_event_callback("blur", "onButtonBlur");

    EXPECT_EQ(btn.event_callback("click"), "onButtonClick");
    EXPECT_EQ(btn.event_callback("focus"), "onButtonFocus");
    EXPECT_EQ(btn.event_callback("blur"), "onButtonBlur");
}

// 测试未设置的回调返回空字符串
TEST(EventBridgeTest, UnsetEventCallback) {
    Button btn("Btn1", "Click Me");
    EXPECT_TRUE(btn.event_callback("click").empty());
    EXPECT_TRUE(btn.event_callback("nonexistent").empty());
}

// 测试通过 set_property 设置事件回调
TEST(EventBridgeTest, SetEventViaProperty) {
    Button btn("Btn1", "Click Me");
    btn.set_property("onClick", "onStartClick");
    btn.set_property("onChange", "onSliderChanged");

    EXPECT_EQ(btn.event_callback("click"), "onStartClick");
    EXPECT_EQ(btn.event_callback("change"), "onSliderChanged");
}

// 测试事件回调在控件树中传递
TEST(EventBridgeTest, EventCallbackInWidgetTree) {
    Panel* root = new Panel("Root");
    Button* btn1 = new Button("Btn1", "Start");
    Button* btn2 = new Button("Btn2", "Exit");

    btn1->set_event_callback("click", "onStartClick");
    btn2->set_event_callback("click", "onExitClick");

    root->add_child(btn1);
    root->add_child(btn2);

    EXPECT_EQ(btn1->event_callback("click"), "onStartClick");
    EXPECT_EQ(btn2->event_callback("click"), "onExitClick");

    // 事件回调在控件树中独立
    EXPECT_NE(btn1->event_callback("click"), btn2->event_callback("click"));

    delete root;
}

// 测试不同类型控件的事件回调
TEST(EventBridgeTest, EventCallbackOnDifferentWidgets) {
    Button btn("Btn", "OK");
    Slider slider("Sld");
    TextInput input("Input");
    ComboBox combo("Combo");

    btn.set_event_callback("click", "onBtnClick");
    slider.set_event_callback("change", "onSliderChange");
    input.set_event_callback("change", "onInputChange");
    combo.set_event_callback("change", "onComboChange");

    EXPECT_EQ(btn.event_callback("click"), "onBtnClick");
    EXPECT_EQ(slider.event_callback("change"), "onSliderChange");
    EXPECT_EQ(input.event_callback("change"), "onInputChange");
    EXPECT_EQ(combo.event_callback("change"), "onComboChange");
}

// 测试事件回调覆盖
TEST(EventBridgeTest, EventCallbackOverwrite) {
    Button btn("Btn", "OK");
    btn.set_event_callback("click", "onClick1");
    EXPECT_EQ(btn.event_callback("click"), "onClick1");

    // 覆盖已有回调
    btn.set_event_callback("click", "onClick2");
    EXPECT_EQ(btn.event_callback("click"), "onClick2");
}

// 测试 check_box 的 toggle 事件
TEST(EventBridgeTest, CheckBoxToggleEvent) {
    CheckBox cb("Cb1", "Option");
    cb.set_event_callback("change", "onCheckChange");
    EXPECT_EQ(cb.event_callback("change"), "onCheckChange");

    // 模拟 toggle
    cb.toggle();
    EXPECT_TRUE(cb.checked());
    cb.toggle();
    EXPECT_FALSE(cb.checked());
}

// 测试 radio_button 的事件
TEST(EventBridgeTest, RadioButtonEvent) {
    RadioButton rb("Rb1", "Choice");
    rb.set_event_callback("change", "onRadioChange");
    EXPECT_EQ(rb.event_callback("change"), "onRadioChange");

    // 模拟 toggle
    rb.toggle();
    EXPECT_TRUE(rb.checked());
}

// ============================================================================
// ScriptVM 与 UIManager 集成测试
// ============================================================================

// 测试 UIManager 创建时 ScriptVM 初始化（无 Renderer 时）
TEST(ScriptVMIntegrationTest, UIManagerCreatesScriptVM) {
    UIManager* mgr = UIManager::create(nullptr);
    ASSERT_NE(mgr, nullptr);

    // 没有 Renderer 时 ScriptVM 也应该被初始化
    // （ScriptVM 不依赖 Renderer）
    // 注意：UIManager 构造函数中 ScriptVM 初始化在 UIRenderer 之后

    mgr->destroy();
}

// 测试 engine.* 在 js eval 中可用
TEST(ScriptVMIntegrationTest, EngineAPIInScript) {
    ScriptVM vm;
    ASSERT_TRUE(vm.init());

    // 注册 engine.*
    EngineBridge::init(&vm, nullptr);

    // 验证 engine 对象
    ScriptResult sr = vm.eval("engine.loadScene.toString()");
    EXPECT_TRUE(sr.success);

    vm.shutdown();
}

// 测试 Widget 的 event_callback 持久性
TEST(EventBridgeTest, EventCallbackPersistence) {
    Button btn("Btn", "OK");
    btn.set_event_callback("click", "myCallback");

    // 即使在样式修改后，回调仍应保持
    btn.set_style_class("primary");
    EXPECT_EQ(btn.event_callback("click"), "myCallback");

    // 即使在位置修改后，回调仍应保持
    btn.set_position(100, 200);
    EXPECT_EQ(btn.event_callback("click"), "myCallback");
}

// 测试 JS 调用 engine.* 时传递错误参数
TEST_F(EngineBridgeTest, EngineAPICalledWithWrongArgs) {
    // 无参数调用 loadScene
    ScriptResult sr = vm_->eval("try { engine.loadScene(); 'ok' } catch(e) { e.message }");
    EXPECT_TRUE(sr.success);
    // 应该有错误信息
    EXPECT_NE(sr.result, "ok");
}

// 测试 engine.* 链式调用
TEST_F(EngineBridgeTest, EngineChainAccess) {
    ScriptResult sr = vm_->eval("var e = engine; e.loadScene; e.playSound;");
    EXPECT_TRUE(sr.success);
}

// 测试 engine.* 在原型链中不被覆盖
TEST_F(EngineBridgeTest, EnginePrototypeIntact) {
    ScriptResult sr = vm_->eval(
        "var keys = Object.keys(engine);"
        "keys.indexOf('loadScene') >= 0 && keys.indexOf('playSound') >= 0;"
    );
    EXPECT_TRUE(sr.success);
    EXPECT_EQ(sr.result, "true");
}

// 测试 engine.* 所有方法调用后返回 undefined
TEST_F(EngineBridgeTest, EngineMethodsReturnUndefined) {
    // 不依赖 UIManager 的方法应返回 undefined
    ScriptResult sr = vm_->eval(
        "var r1 = engine.loadScene('test.gesc');"
        "var r2 = engine.playSound('test.wav');"
        "var r3 = engine.getString('key');"
        "var r4 = engine.bind('evt', function(){});"
        "r1 === undefined && r2 === undefined && r3 === 'key' && r4 === undefined"
    );
    EXPECT_TRUE(sr.success);
    EXPECT_EQ(sr.result, "true");
}

// 测试多次调用 engine.* 不崩溃
TEST_F(EngineBridgeTest, RepeatedEngineCalls) {
    for (int i = 0; i < 100; i++) {
        ScriptResult sr = vm_->eval("engine.loadScene('test.gesc')");
        EXPECT_TRUE(sr.success);
    }
}

// 测试 engine.* 方法在 JS 异常时仍保持可用
TEST_F(EngineBridgeTest, EngineMethodsAfterJSError) {
    ScriptResult sr = vm_->eval("throw new Error('test error')");
    EXPECT_FALSE(sr.success); // 有错误
    
    // 引擎方法仍可用
    sr = vm_->eval("typeof engine.loadScene");
    EXPECT_TRUE(sr.success);
    EXPECT_EQ(sr.result, "function");
}

// 测试 engine.getString 返回字符串
TEST_F(EngineBridgeTest, GetStringReturnsString) {
    ScriptResult sr = vm_->eval("typeof engine.getString('menu.start')");
    EXPECT_TRUE(sr.success);
    EXPECT_EQ(sr.result, "string");
}

// 测试 engine.getString 空字符串
TEST_F(EngineBridgeTest, GetStringEmptyKey) {
    ScriptResult sr = vm_->eval("engine.getString('')");
    EXPECT_TRUE(sr.success);
    EXPECT_EQ(sr.result, "");
}

// 测试 engine.bind 接受非函数参数（当前实现未完全验证类型）
TEST_F(EngineBridgeTest, BindAcceptsNonFunction) {
    ScriptResult sr = vm_->eval(
        "try { engine.bind('test', 'not_a_function'); 'ok' } catch(e) { 'error:' + e.message }");
    EXPECT_TRUE(sr.success);
    // bind 目前是预留实现，不验证回调类型，应该返回 ok
    EXPECT_EQ(sr.result, "ok");
}

// ============================================================================
// 端到端 JS 事件桥接集成测试
// ============================================================================
//
// 这些测试验证 UI 事件（点击、焦点、悬停）能够正确触发 JS 回调函数。
// 使用真实的 UIManager + ScriptVM + Widget 树进行端到端测试。

class JSEventBridgeTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 创建 UIManager（无 Renderer，ScriptVM 仍然初始化）
        mgr_ = UIManager::create(nullptr);
        ASSERT_NE(mgr_, nullptr);

        ScriptVM* vm = EngineBridge::vm();
        ASSERT_NE(vm, nullptr);
        ASSERT_TRUE(vm->initialized());

        // 注册测试用的 JS 回调函数到全局作用域
        // 使用 _cb 前缀变量跟踪调用状态
        ScriptResult sr = vm->eval(
            "var _cbCalled = '';\n"
            "var _cbArg = '';\n"
            "function onTestClick(id) { _cbCalled = 'click'; _cbArg = id; }\n"
            "function onTestFocus(id) { _cbCalled = 'focus'; _cbArg = id; }\n"
            "function onTestBlur(id) { _cbCalled = 'blur'; _cbArg = id; }\n"
            "function onTestHoverEnter(id) { _cbCalled = 'hover_enter'; _cbArg = id; }\n"
            "function onTestHoverLeave(id) { _cbCalled = 'hover_leave'; _cbArg = id; }\n"
        );
        ASSERT_TRUE(sr.success)
            << "Failed to register test JS callbacks: " << sr.error_msg;

        // 创建控件树
        // RootPanel: 全屏背景，不接收鼠标事件
        root_ = new Panel("RootPanel");
        root_->set_bounds(Rect{0, 0, 800, 600});

        // Button: 可交互控件，位于 (100, 100) 处，尺寸 200x50
        btn_ = new Button("TestBtn", "Click Me");
        btn_->set_position(100, 100);
        btn_->set_size(200, 50);
        btn_->set_bounds(Rect{100, 100, 200, 50});
        root_->add_child(btn_);

        mgr_->set_root(root_);
    }

    void TearDown() override {
        if (mgr_) {
            mgr_->destroy();
            mgr_ = nullptr;
        }
    }

    // 查询 JS 全局变量 _cbCalled（最近触发的事件名）
    std::string cbCalled() {
        ScriptResult sr = EngineBridge::vm()->eval("_cbCalled");
        return sr.success ? sr.result : "";
    }

    // 查询 JS 全局变量 _cbArg（传递给回调的 widget id）
    std::string cbArg() {
        ScriptResult sr = EngineBridge::vm()->eval("_cbArg");
        return sr.success ? sr.result : "";
    }

    // 重置 JS 调用状态
    void resetJsState() {
        EngineBridge::vm()->eval("_cbCalled = ''; _cbArg = '';");
    }

    UIManager* mgr_ = nullptr;
    Widget* root_ = nullptr;
    Button* btn_ = nullptr;
};

// 点击事件触发 JS 回调
TEST_F(JSEventBridgeTest, ClickEventFiresJSCallback) {
    btn_->set_event_callback("click", "onTestClick");

    // 先移动到按钮位置
    mgr_->on_mouse_move(150, 125);
    // 模拟鼠标按下和释放
    mgr_->on_mouse_button(0, true);
    mgr_->on_mouse_button(0, false);

    EXPECT_EQ(cbCalled(), "click");
    EXPECT_EQ(cbArg(), "TestBtn");
}

// 焦点事件触发 JS 回调
TEST_F(JSEventBridgeTest, FocusEventFiresJSCallback) {
    btn_->set_event_callback("focus", "onTestFocus");

    mgr_->set_focus(btn_);

    EXPECT_EQ(cbCalled(), "focus");
    EXPECT_EQ(cbArg(), "TestBtn");
}

// 失焦事件触发 JS 回调
TEST_F(JSEventBridgeTest, BlurEventFiresJSCallback) {
    btn_->set_event_callback("blur", "onTestBlur");

    // 先聚焦
    mgr_->set_focus(btn_);
    resetJsState();
    // 切换到其他控件触发 blur（传入 nullptr 等同于清除焦点）
    mgr_->set_focus(nullptr);

    EXPECT_EQ(cbCalled(), "blur");
    EXPECT_EQ(cbArg(), "TestBtn");
}

// 悬停进入事件触发 JS 回调
TEST_F(JSEventBridgeTest, HoverEnterEventFiresJSCallback) {
    btn_->set_event_callback("hover_enter", "onTestHoverEnter");

    // 鼠标移动到按钮区域内
    mgr_->on_mouse_move(150, 125);

    EXPECT_EQ(cbCalled(), "hover_enter");
    EXPECT_EQ(cbArg(), "TestBtn");
}

// 悬停离开事件触发 JS 回调
TEST_F(JSEventBridgeTest, HoverLeaveEventFiresJSCallback) {
    btn_->set_event_callback("hover_leave", "onTestHoverLeave");

    // 先进入按钮区域
    mgr_->on_mouse_move(150, 125);
    resetJsState();

    // 移出按钮区域
    mgr_->on_mouse_move(0, 0);

    EXPECT_EQ(cbCalled(), "hover_leave");
    EXPECT_EQ(cbArg(), "TestBtn");
}

// 多个事件类型同时工作
TEST_F(JSEventBridgeTest, MultipleEventTypes) {
    btn_->set_event_callback("click", "onTestClick");
    btn_->set_event_callback("focus", "onTestFocus");
    btn_->set_event_callback("blur", "onTestBlur");

    // 点击触发 focus（在 on_mouse_button down 中） + click（在 on_mouse_button up 中）
    resetJsState();
    mgr_->on_mouse_move(150, 125);
    mgr_->on_mouse_button(0, true);
    mgr_->on_mouse_button(0, false);
    // click 是最后一个触发的事件
    EXPECT_EQ(cbCalled(), "click");

    // 失焦触发 blur
    resetJsState();
    mgr_->set_focus(nullptr);
    EXPECT_EQ(cbCalled(), "blur");
}

// 未设置 JS 回调时事件不崩溃
TEST_F(JSEventBridgeTest, EventWithoutCallbackNoCrash) {
    // 不设置任何事件回调
    mgr_->on_mouse_move(150, 125);
    mgr_->on_mouse_button(0, true);
    mgr_->on_mouse_button(0, false);
    mgr_->set_focus(btn_);
    mgr_->set_focus(nullptr);
    // 应该不崩溃
}

// 引用未定义的 JS 函数名不崩溃
TEST_F(JSEventBridgeTest, UndefinedCallbackNoCrash) {
    btn_->set_event_callback("click", "nonExistentFunction");
    btn_->set_event_callback("focus", "nonExistentFunction");
    btn_->set_event_callback("hover_enter", "nonExistentFunction");

    mgr_->on_mouse_move(150, 125);
    mgr_->on_mouse_button(0, true);
    mgr_->on_mouse_button(0, false);
    mgr_->set_focus(btn_);
    // 应该不崩溃
}

// 点击事件按正确的顺序触发（先 focus 后 click）
TEST_F(JSEventBridgeTest, ClickEventOrder) {
    btn_->set_event_callback("click", "onTestClick");
    btn_->set_event_callback("focus", "onTestFocus");

    // 记录事件序列
    ScriptVM* vm = EngineBridge::vm();
    vm->eval("var _eventSeq = [];");
    vm->eval(
        "var _origOnTestClick = onTestClick;"
        "onTestClick = function(id) { _eventSeq.push('click'); _origOnTestClick(id); };"
    );
    vm->eval(
        "var _origOnTestFocus = onTestFocus;"
        "onTestFocus = function(id) { _eventSeq.push('focus'); _origOnTestFocus(id); };"
    );

    mgr_->on_mouse_move(150, 125);
    mgr_->on_mouse_button(0, true);
    mgr_->on_mouse_button(0, false);

    // 验证事件顺序：focus 先于 click
    ScriptResult sr = vm->eval("_eventSeq.join(',')");
    EXPECT_TRUE(sr.success);
    EXPECT_EQ(sr.result, "focus,click");
}

// 悬停事件序列：进入 -> 离开
TEST_F(JSEventBridgeTest, HoverEventSequence) {
    btn_->set_event_callback("hover_enter", "onTestHoverEnter");
    btn_->set_event_callback("hover_leave", "onTestHoverLeave");

    ScriptVM* vm = EngineBridge::vm();
    vm->eval("var _eventSeq = [];");
    vm->eval(
        "var _origEnter = onTestHoverEnter;"
        "onTestHoverEnter = function(id) { _eventSeq.push('enter'); _origEnter(id); };"
    );
    vm->eval(
        "var _origLeave = onTestHoverLeave;"
        "onTestHoverLeave = function(id) { _eventSeq.push('leave'); _origLeave(id); };"
    );

    // 进入
    mgr_->on_mouse_move(150, 125);
    // 离开
    mgr_->on_mouse_move(0, 0);

    ScriptResult sr = vm->eval("_eventSeq.join(',')");
    EXPECT_TRUE(sr.success);
    EXPECT_EQ(sr.result, "enter,leave");
}

// 多次点击累积触发 JS 回调
TEST_F(JSEventBridgeTest, RepeatedClickEvents) {
    btn_->set_event_callback("click", "onTestClick");

    mgr_->on_mouse_move(150, 125);
    for (int i = 0; i < 5; i++) {
        resetJsState();
        mgr_->on_mouse_button(0, true);
        mgr_->on_mouse_button(0, false);
        EXPECT_EQ(cbCalled(), "click") << "Failed on click #" << i;
        EXPECT_EQ(cbArg(), "TestBtn") << "Failed on arg check #" << i;
    }
}

// engine.showDialog 在有 UIManager 时正常工作
TEST_F(JSEventBridgeTest, ShowDialogWithUIManager) {
    Panel* dialog = new Panel("MyDialog");
    dialog->set_visible(false);
    root_->add_child(dialog);

    ScriptResult sr = EngineBridge::vm()->eval("engine.showDialog('MyDialog')");
    EXPECT_TRUE(sr.success);
    EXPECT_TRUE(dialog->visible());

    // 清理：先从 root 的子控件树移除，再 pop_modal
    // 顺序重要：先 remove_child 避免 double-free
    root_->remove_child(dialog);
    mgr_->pop_modal(); // 从 modals_ 移除并 delete dialog
}

// engine.closeDialog 在有 UIManager 时正常工作
TEST_F(JSEventBridgeTest, CloseDialogWithUIManager) {
    Panel* dialog = new Panel("MyDialog");
    // 不添加到 root（避免双删），直接 push 到模态栈
    mgr_->push_modal(dialog);

    ScriptResult sr = EngineBridge::vm()->eval("engine.closeDialog()");
    EXPECT_TRUE(sr.success);
    // pop_modal 已删除 dialog，指针已失效
}

// 先 showDialog 再 closeDialog 的完整流程
TEST_F(JSEventBridgeTest, ShowAndCloseDialog) {
    Panel* dialog = new Panel("MyDialog");
    dialog->set_visible(false);
    // 不添加到 root，直接操作
    mgr_->push_modal(dialog);

    // 从 JS 调用 closeDialog
    ScriptResult sr = EngineBridge::vm()->eval("engine.closeDialog()");
    EXPECT_TRUE(sr.success);
    // dialog 已被 delete
}

// 连续调用 showDialog 和 closeDialog 不崩溃
TEST_F(JSEventBridgeTest, RepeatedDialogCalls) {
    for (int i = 0; i < 3; i++) {
        std::string dlg_id = "Dlg" + std::to_string(i);
        Panel* dlg = new Panel(dlg_id.c_str());
        // 直接 push 到模态栈
        mgr_->push_modal(dlg);
    }

    // 关闭所有模态
    for (int i = 0; i < 3; i++) {
        ScriptResult sr = EngineBridge::vm()->eval("engine.closeDialog()");
        EXPECT_TRUE(sr.success);
    }
}

// 空模态栈时调用 closeDialog 不崩溃
TEST_F(JSEventBridgeTest, CloseDialogEmptyStack) {
    ScriptResult sr = EngineBridge::vm()->eval("engine.closeDialog()");
    EXPECT_TRUE(sr.success);
    // 空栈时 pop_modal 不操作，不崩溃
}

// 多个控件各自有独立的事件回调
TEST_F(JSEventBridgeTest, MultipleWidgetsIndependentCallbacks) {
    Button* btn2 = new Button("SecondBtn", "Cancel");
    btn2->set_position(100, 200);
    btn2->set_size(200, 50);
    btn2->set_bounds(Rect{100, 200, 200, 50});
    root_->add_child(btn2);

    btn_->set_event_callback("click", "onTestClick");
    btn2->set_event_callback("click", "onTestClick");

    // 点击第一个按钮
    resetJsState();
    mgr_->on_mouse_move(150, 125);
    mgr_->on_mouse_button(0, true);
    mgr_->on_mouse_button(0, false);
    EXPECT_EQ(cbArg(), "TestBtn");

    // 点击第二个按钮
    resetJsState();
    mgr_->on_mouse_move(150, 225);
    mgr_->on_mouse_button(0, true);
    mgr_->on_mouse_button(0, false);
    EXPECT_EQ(cbArg(), "SecondBtn");
}

// 焦点和悬停事件同时设置
TEST_F(JSEventBridgeTest, FocusAndHoverTogether) {
    btn_->set_event_callback("focus", "onTestFocus");
    btn_->set_event_callback("hover_enter", "onTestHoverEnter");

    // 悬停触发 hover_enter
    mgr_->on_mouse_move(150, 125);
    EXPECT_EQ(cbCalled(), "hover_enter");

    // 点击触发 focus + click
    resetJsState();
    mgr_->on_mouse_button(0, true);
    mgr_->on_mouse_button(0, false);
    // 最后触发的是 click（因为没设置 click 回调，但 focus 在 down 时触发）
    // 实际上在 on_mouse_button(0, true) 中调用了 set_focus，触发 focus 事件
    // 然后在 on_mouse_button(0, false) 中触发 click
    // 由于没有设置 click 回调，最后一个触发的事件是 click（但 fire_js_event 被跳过因回调为空）
    // 所以 cbCalled 应该还是 "focus"
    // 实际上，在 on_mouse_button(0, true) 中 set_focus 触发 focus，cbCalled 被设为 "focus"
    // 然后在 on_mouse_button(0, false) 中，fire_js_event("click") 被调用，但回调为空所以跳过
    // 所以 cbCalled 保持 "focus"
    EXPECT_EQ(cbCalled(), "focus");
}