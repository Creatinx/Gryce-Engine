#include <gtest/gtest.h>

#include <string>

#include "ui/script_vm.h"

using namespace GryceEngineUtils::ui;

// ScriptVM 测试套件
class ScriptVMTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(vm.init()) << "ScriptVM failed to initialize";
    }

    void TearDown() override {
        vm.shutdown();
    }

    ScriptVM vm;
};

// ============================================================================
// 基本初始化和关闭
// ============================================================================

TEST_F(ScriptVMTest, InitAndShutdown) {
    EXPECT_TRUE(vm.initialized());
    vm.shutdown();
    EXPECT_FALSE(vm.initialized());
}

// ============================================================================
// 表达式求值
// ============================================================================

TEST_F(ScriptVMTest, EvalNumericExpression) {
    ScriptResult sr = vm.eval("1 + 2 * 3");
    EXPECT_TRUE(sr.success);
    EXPECT_EQ(sr.result, "7");
}

TEST_F(ScriptVMTest, EvalStringExpression) {
    ScriptResult sr = vm.eval("\"Hello\" + \" \" + \"World\"");
    EXPECT_TRUE(sr.success);
    EXPECT_EQ(sr.result, "Hello World");
}

TEST_F(ScriptVMTest, EvalBooleanExpression) {
    ScriptResult sr = vm.eval("2 > 1");
    EXPECT_TRUE(sr.success);
    EXPECT_EQ(sr.result, "true");
}

TEST_F(ScriptVMTest, EvalUndefined) {
    ScriptResult sr = vm.eval("undefined");
    EXPECT_TRUE(sr.success);
    EXPECT_EQ(sr.result, "undefined");
}

TEST_F(ScriptVMTest, EvalNull) {
    ScriptResult sr = vm.eval("null");
    EXPECT_TRUE(sr.success);
    EXPECT_EQ(sr.result, "null");
}

// ============================================================================
// 错误处理
// ============================================================================

TEST_F(ScriptVMTest, SyntaxError) {
    ScriptResult sr = vm.eval("this is not valid javascript");
    EXPECT_FALSE(sr.success);
    EXPECT_FALSE(sr.error_msg.empty());
}

TEST_F(ScriptVMTest, RuntimeError) {
    ScriptResult sr = vm.eval("throw new Error('boom')");
    EXPECT_FALSE(sr.success);
    EXPECT_NE(sr.error_msg.find("boom"), std::string::npos)
        << "error should surface message, got: " << sr.error_msg;
}

TEST_F(ScriptVMTest, ReferenceError) {
    ScriptResult sr = vm.eval("nonexistent_function()");
    EXPECT_FALSE(sr.success);
    EXPECT_FALSE(sr.error_msg.empty());
}

// ============================================================================
// 状态持久性（跨 eval 调用）
// ============================================================================

TEST_F(ScriptVMTest, StatePersistsAcrossEvalCalls) {
    ScriptResult sr1 = vm.eval("var x = 42;");
    EXPECT_TRUE(sr1.success);

    ScriptResult sr2 = vm.eval("x");
    EXPECT_TRUE(sr2.success);
    EXPECT_EQ(sr2.result, "42");
}

TEST_F(ScriptVMTest, FunctionDefinedInOneEvalCalledInAnother) {
    ScriptResult sr1 = vm.eval("function add(a, b) { return a + b; }");
    EXPECT_TRUE(sr1.success);

    ScriptResult sr2 = vm.eval("add(3, 4)");
    EXPECT_TRUE(sr2.success);
    EXPECT_EQ(sr2.result, "7");
}

// ============================================================================
// CallFunction
// ============================================================================

TEST_F(ScriptVMTest, CallFunctionNoArgs) {
    vm.eval("function greet() { return 'hello'; }");

    ScriptResult sr = vm.call_function("greet");
    EXPECT_TRUE(sr.success);
    EXPECT_EQ(sr.result, "hello");
}

TEST_F(ScriptVMTest, CallFunctionWithIntArgs) {
    vm.eval("function multiply(a, b) { return a * b; }");

    ScriptResult sr = vm.call_function("multiply", {JSValueWrapper(6), JSValueWrapper(7)});
    EXPECT_TRUE(sr.success);
    EXPECT_EQ(sr.result, "42");
}

TEST_F(ScriptVMTest, CallFunctionWithStringArgs) {
    vm.eval("function concat(a, b) { return a + b; }");

    ScriptResult sr = vm.call_function("concat", {JSValueWrapper("Hello, "), JSValueWrapper("World!")});
    EXPECT_TRUE(sr.success);
    EXPECT_EQ(sr.result, "Hello, World!");
}

TEST_F(ScriptVMTest, CallFunctionWithFloatArgs) {
    vm.eval("function divide(a, b) { return a / b; }");

    ScriptResult sr = vm.call_function("divide", {JSValueWrapper(10.0), JSValueWrapper(3.0)});
    EXPECT_TRUE(sr.success);
    // 10/3 = 3.333...  在 JS 中浮点数结果
    EXPECT_NE(sr.result.find("3.33"), std::string::npos);
}

TEST_F(ScriptVMTest, CallFunctionWithBoolArgs) {
    vm.eval("function invert(v) { return !v; }");

    ScriptResult sr = vm.call_function("invert", {JSValueWrapper(true)});
    EXPECT_TRUE(sr.success);
    EXPECT_EQ(sr.result, "false");
}

TEST_F(ScriptVMTest, CallFunctionNotFound) {
    ScriptResult sr = vm.call_function("doesNotExist");
    EXPECT_FALSE(sr.success);
    EXPECT_FALSE(sr.error_msg.empty());
}

// ============================================================================
// RegisterFunction（C++ 函数注册到 JS）
// ============================================================================

// C 函数：JS 加法
static JSValue js_add(JSContext* ctx, JSValueConst this_val,
                       int argc, JSValueConst* argv) {
    if (argc < 2) return JS_UNDEFINED;
    double a, b;
    JS_ToFloat64(ctx, &a, argv[0]);
    JS_ToFloat64(ctx, &b, argv[1]);
    return JS_NewFloat64(ctx, a + b);
}

TEST_F(ScriptVMTest, RegisterAndCallCFunction) {
    vm.register_function("cpp_add", js_add);

    ScriptResult sr = vm.eval("cpp_add(10, 20)");
    EXPECT_TRUE(sr.success);
    EXPECT_EQ(sr.result, "30");
}

// C 函数：返回字符串
static JSValue js_greet(JSContext* ctx, JSValueConst this_val,
                         int argc, JSValueConst* argv) {
    return JS_NewString(ctx, "Hello from C++!");
}

TEST_F(ScriptVMTest, RegisterFunctionReturnsString) {
    vm.register_function("cpp_greet", js_greet);

    ScriptResult sr = vm.eval("cpp_greet()");
    EXPECT_TRUE(sr.success);
    EXPECT_EQ(sr.result, "Hello from C++!");
}

// ============================================================================
// console.log 重定向
// ============================================================================

TEST_F(ScriptVMTest, ConsoleLogDoesNotCrash) {
    ScriptResult sr = vm.eval("console.log('test message')");
    EXPECT_TRUE(sr.success);
}

TEST_F(ScriptVMTest, ConsoleLogMultipleArgs) {
    ScriptResult sr = vm.eval("console.log('a', 'b', 'c')");
    EXPECT_TRUE(sr.success);
}

TEST_F(ScriptVMTest, ConsoleWarn) {
    ScriptResult sr = vm.eval("console.warn('warning message')");
    EXPECT_TRUE(sr.success);
}

TEST_F(ScriptVMTest, ConsoleError) {
    ScriptResult sr = vm.eval("console.error('error message')");
    EXPECT_TRUE(sr.success);
}

// ============================================================================
// 高级 JS 功能
// ============================================================================

TEST_F(ScriptVMTest, ArrayOperations) {
    ScriptResult sr = vm.eval("var arr = [3, 1, 4, 1, 5]; arr.sort(); arr.join(',')");
    EXPECT_TRUE(sr.success);
    EXPECT_EQ(sr.result, "1,1,3,4,5");
}

TEST_F(ScriptVMTest, ObjectOperations) {
    ScriptResult sr = vm.eval("var obj = {a: 1, b: 2}; obj.a + obj.b");
    EXPECT_TRUE(sr.success);
    EXPECT_EQ(sr.result, "3");
}

TEST_F(ScriptVMTest, StringMethods) {
    ScriptResult sr = vm.eval("'hello world'.toUpperCase()");
    EXPECT_TRUE(sr.success);
    EXPECT_EQ(sr.result, "HELLO WORLD");
}

// ============================================================================
// 未初始化时调用
// ============================================================================

TEST(ScriptVMUninitializedTest, Eval) {
    ScriptVM vm2;
    ScriptResult sr = vm2.eval("1+1");
    EXPECT_FALSE(sr.success);
    EXPECT_EQ(sr.error_msg, "ScriptVM not initialized");
}

TEST(ScriptVMUninitializedTest, CallFunction) {
    ScriptVM vm2;
    ScriptResult sr = vm2.call_function("test");
    EXPECT_FALSE(sr.success);
    EXPECT_EQ(sr.error_msg, "ScriptVM not initialized");
}

// 验证 JSValueWrapper 的默认构造
TEST(ScriptVMValueTest, Default) {
    JSValueWrapper w;
    EXPECT_EQ(w.type, JSValueWrapper::Type::Undefined);
}

// 验证 JSValueWrapper 的各种构造
TEST(ScriptVMValueTest, Types) {
    JSValueWrapper w_bool(true);
    EXPECT_EQ(w_bool.type, JSValueWrapper::Type::Bool);
    EXPECT_TRUE(w_bool.bool_val);

    JSValueWrapper w_int(42);
    EXPECT_EQ(w_int.type, JSValueWrapper::Type::Int);
    EXPECT_EQ(w_int.int_val, 42);

    JSValueWrapper w_float(3.14);
    EXPECT_EQ(w_float.type, JSValueWrapper::Type::Float);
    EXPECT_DOUBLE_EQ(w_float.float_val, 3.14);

    JSValueWrapper w_str("hello");
    EXPECT_EQ(w_str.type, JSValueWrapper::Type::String);
    EXPECT_EQ(w_str.str_val, "hello");
}