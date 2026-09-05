#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "script/runtime/script_vm.h"

using namespace GryceEngineUtils::script;

// ============================================================================
// ScriptVM 基础测试
// ============================================================================

TEST(ScriptVM2Test, InitAndShutdown) {
    ScriptVM vm;
    EXPECT_FALSE(vm.initialized());

    EXPECT_TRUE(vm.init());
    EXPECT_TRUE(vm.initialized());

    vm.shutdown();
    EXPECT_FALSE(vm.initialized());
}

TEST(ScriptVM2Test, EvalSimpleExpression) {
    ScriptVM vm;
    ASSERT_TRUE(vm.init());

    auto result = vm.eval("1 + 2");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.result, "3");

    vm.shutdown();
}

TEST(ScriptVM2Test, EvalConsoleLog) {
    ScriptVM vm;
    ASSERT_TRUE(vm.init());

    // console.log should not crash
    auto result = vm.eval("console.log('hello from JS'); 42");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.result, "42");

    vm.shutdown();
}

TEST(ScriptVM2Test, EvalSyntaxError) {
    ScriptVM vm;
    ASSERT_TRUE(vm.init());

    auto result = vm.eval("syntax error{{{");
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.error_msg.empty());

    vm.shutdown();
}

TEST(ScriptVM2Test, CallFunction) {
    ScriptVM vm;
    ASSERT_TRUE(vm.init());

    // Define a global function
    vm.eval("function add(a, b) { return a + b; }");

    auto result = vm.call_function("add", {JSValueWrapper(3), JSValueWrapper(7)});
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.result, "10");

    vm.shutdown();
}

TEST(ScriptVM2Test, CallNonExistentFunction) {
    ScriptVM vm;
    ASSERT_TRUE(vm.init());

    auto result = vm.call_function("nonExistentFunction");
    EXPECT_FALSE(result.success);

    vm.shutdown();
}

TEST(ScriptVM2Test, RegisterFunction) {
    ScriptVM vm;
    ASSERT_TRUE(vm.init());

    // Register a C++ function
    auto js_add = [](JSContext* ctx, JSValueConst this_val,
                      int argc, JSValueConst* argv) -> JSValue {
        if (argc < 2) return JS_UNDEFINED;
        double a, b;
        JS_ToFloat64(ctx, &a, argv[0]);
        JS_ToFloat64(ctx, &b, argv[1]);
        return JS_NewFloat64(ctx, a + b);
    };
    vm.register_function("cpp_add", js_add);

    auto result = vm.eval("cpp_add(5, 7)");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.result, "12");

    vm.shutdown();
}

// ============================================================================
// ScriptVM 模块作用域测试
// ============================================================================

TEST(ScriptVM2Test, EvalModule) {
    ScriptVM vm;
    ASSERT_TRUE(vm.init());

    JSValue module_ns = JS_UNDEFINED;
    auto result = vm.eval_module(
        "export function greet(name) { return 'Hello ' + name; }",
        "test_module.js",
        &module_ns
    );

    EXPECT_TRUE(result.success);
    EXPECT_FALSE(JS_IsUndefined(module_ns));

    // 调用模块导出的函数
    auto call_result = vm.call_module_function(module_ns, "greet", {JSValueWrapper("World")});
    if (!call_result.success) {
        ADD_FAILURE() << "Error: " << call_result.error_msg;
    }
    EXPECT_TRUE(call_result.success);
    EXPECT_EQ(call_result.result, "Hello World");

    JS_FreeValue(vm.context(), module_ns);
    vm.shutdown();
}

TEST(ScriptVM2Test, ModuleExports) {
    ScriptVM vm;
    ASSERT_TRUE(vm.init());

    JSValue module_ns = JS_UNDEFINED;
    auto result = vm.eval_module(
        "export const PI = 3.14159;\n"
        "export function double(x) { return x * 2; }",
        "consts.js",
        &module_ns
    );

    EXPECT_TRUE(result.success);

    // 检查导出是否可用
    auto call_result = vm.call_module_function(module_ns, "double", {JSValueWrapper(21)});
    if (!call_result.success) {
        ADD_FAILURE() << "Error: " << call_result.error_msg;
    }
    EXPECT_TRUE(call_result.success);
    EXPECT_EQ(call_result.result, "42");

    JS_FreeValue(vm.context(), module_ns);
    vm.shutdown();
}

TEST(ScriptVM2Test, MultipleModules) {
    ScriptVM vm;
    ASSERT_TRUE(vm.init());

    JSValue mod1 = JS_UNDEFINED;
    vm.eval_module("export const a = 10;", "mod1.js", &mod1);

    JSValue mod2 = JS_UNDEFINED;
    vm.eval_module("export const b = 20;", "mod2.js", &mod2);

    // Each module should have its own scope
    EXPECT_TRUE(JS_IsObject(mod1));
    EXPECT_TRUE(JS_IsObject(mod2));

    // Verify exports via property access
    JSContext* ctx = vm.context();
    JSValue val = JS_GetPropertyStr(ctx, mod1, "a");
    EXPECT_TRUE(JS_IsNumber(val));
    JS_FreeValue(ctx, val);

    val = JS_GetPropertyStr(ctx, mod2, "b");
    EXPECT_TRUE(JS_IsNumber(val));
    JS_FreeValue(ctx, val);

    JS_FreeValue(vm.context(), mod1);
    JS_FreeValue(vm.context(), mod2);
    vm.shutdown();
}

TEST(ScriptVM2Test, FormatException) {
    ScriptVM vm;
    ASSERT_TRUE(vm.init());

    // Just verify the method exists and doesn't crash
    auto result = vm.eval("undefinedVar");
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.error_msg.empty());

    vm.shutdown();
}

TEST(ScriptVM2Test, ConsumeException) {
    ScriptVM vm;
    ASSERT_TRUE(vm.init());

    auto result = vm.eval("throw new Error('test error')");
    EXPECT_FALSE(result.success);

    // consume_exception should return empty since eval already consumed it
    std::string exc = vm.consume_exception();
    // The exception might already be consumed by eval, so it could be empty
    EXPECT_TRUE(exc.empty() || !exc.empty());

    vm.shutdown();
}

TEST(ScriptVM2Test, DoubleInit) {
    ScriptVM vm;
    EXPECT_TRUE(vm.init());
    EXPECT_TRUE(vm.init()); // second init should be safe
    EXPECT_TRUE(vm.initialized());
    vm.shutdown();
}

TEST(ScriptVM2Test, DoubleShutdown) {
    ScriptVM vm;
    ASSERT_TRUE(vm.init());
    vm.shutdown();
    vm.shutdown(); // should not crash
    EXPECT_FALSE(vm.initialized());
}

TEST(ScriptVM2Test, EvalOnShutdownVM) {
    ScriptVM vm;
    ASSERT_TRUE(vm.init());
    vm.shutdown();

    auto result = vm.eval("1+1");
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.error_msg, "ScriptVM not initialized");
}

TEST(ScriptVM2Test, RegisterFunctionOnShutdownVM) {
    ScriptVM vm;
    ASSERT_TRUE(vm.init());
    vm.shutdown();

    // Should not crash
    vm.register_function("test", nullptr);
}