#include <gtest/gtest.h>

#include <string>

#include "lua2js.h"
#include "script/runtime/script_vm.h"

using lua2js::convert;

namespace {

std::string convert_ok(const std::string& lua) {
    auto r = convert(lua);
    EXPECT_TRUE(r.ok) << "convert failed";
    return r.output;
}

// 转换并在 QuickJS 模块中执行，验证语义一致
std::string convert_and_run(const std::string& lua, const std::string& fn,
                            std::initializer_list<GryceEngineUtils::script::JSValueWrapper> args = {}) {
    auto r = convert(lua);
    EXPECT_TRUE(r.ok);
    GryceEngineUtils::script::ScriptVM vm;
    if (!vm.init()) return "";
    JSValue module_ns = JS_UNDEFINED;
    auto er = vm.eval_module(r.output, "converted.lua.js", &module_ns);
    EXPECT_TRUE(er.success) << "eval failed: " << er.error_msg << "\n---\n" << r.output;
    if (!er.success) {
        vm.shutdown();
        return "";
    }
    std::vector<GryceEngineUtils::script::JSValueWrapper> arg_vec(args);
    auto cr = vm.call_module_function(module_ns, fn.c_str(), arg_vec);
    std::string result;
    if (cr.success) result = cr.result;
    else ADD_FAILURE() << "call failed: " << cr.error_msg;
    JS_FreeValue(vm.context(), module_ns);
    vm.shutdown();
    return result;
}

} // namespace

// ============================================================================
// 注释
// ============================================================================

TEST(Lua2JsTest, LineComment) {
    const std::string out = convert_ok("-- 这是注释\nlocal x = 1\n");
    EXPECT_NE(out.find("// 这是注释"), std::string::npos);
    EXPECT_NE(out.find("let x = 1"), std::string::npos);
}

TEST(Lua2JsTest, BlockComment) {
    const std::string out = convert_ok("--[[ 块注释\n跨行\n]]\nlocal x = 1\n");
    EXPECT_NE(out.find("/*"), std::string::npos);
    EXPECT_NE(out.find("*/"), std::string::npos);
    EXPECT_NE(out.find("let x = 1"), std::string::npos);
}

// ============================================================================
// 函数
// ============================================================================

TEST(Lua2JsTest, TopLevelFunction) {
    const std::string out = convert_ok("function add(a, b)\n  return a + b\nend\n");
    EXPECT_NE(out.find("export function add(a, b) {"), std::string::npos);
}

TEST(Lua2JsTest, LocalFunction) {
    const std::string out = convert_ok("local function twice(x)\n  return x * 2\nend\n");
    EXPECT_NE(out.find("function twice(x) {"), std::string::npos);
}

TEST(Lua2JsTest, TableMethodFunction) {
    const std::string out = convert_ok("function Player.new(name)\n  return name\nend\n");
    EXPECT_NE(out.find("Player.new = function(name) {"), std::string::npos);
}

// ============================================================================
// 变量与表构造
// ============================================================================

TEST(Lua2JsTest, LocalVariable) {
    const std::string out = convert_ok("local hp = 100\n");
    EXPECT_NE(out.find("let hp = 100"), std::string::npos);
}

TEST(Lua2JsTest, TableConstruct) {
    const std::string out = convert_ok("local cfg = {speed = 10, name = \"p\"}\n");
    EXPECT_NE(out.find("{speed: 10, name: \"p\"}"), std::string::npos);
}

TEST(Lua2JsTest, TableConstructBracketKey) {
    const std::string out = convert_ok("local t = {[1] = \"a\"}\n");
    EXPECT_NE(out.find("{[1]: \"a\"}"), std::string::npos);
}

// ============================================================================
// 运算符
// ============================================================================

TEST(Lua2JsTest, Operators) {
    const std::string out = convert_ok("local r = a and b or not c\nlocal e = a ~= b\nlocal s = a .. b\nlocal n = nil\n");
    EXPECT_NE(out.find("a && b || ! c"), std::string::npos);
    EXPECT_NE(out.find("a !== b"), std::string::npos);
    EXPECT_NE(out.find("a + b"), std::string::npos);
    EXPECT_NE(out.find("null"), std::string::npos);
}

TEST(Lua2JsTest, LengthOperator) {
    const std::string out = convert_ok("local n = #items\n");
    EXPECT_NE(out.find("__len(items)"), std::string::npos);
    EXPECT_NE(out.find("function __len("), std::string::npos);
}

// ============================================================================
// 控制流
// ============================================================================

TEST(Lua2JsTest, IfElseIfElse) {
    const std::string out = convert_ok(
        "function grade(v)\n"
        "  if v >= 90 then\n"
        "    return \"A\"\n"
        "  elseif v >= 60 then\n"
        "    return \"B\"\n"
        "  else\n"
        "    return \"C\"\n"
        "  end\n"
        "end\n");
    EXPECT_NE(out.find("if (v >= 90) {"), std::string::npos);
    EXPECT_NE(out.find("} else if (v >= 60) {"), std::string::npos);
    EXPECT_NE(out.find("} else {"), std::string::npos);
}

TEST(Lua2JsTest, NumericFor) {
    const std::string out = convert_ok("for i = 1, 10 do\n  print(i)\nend\n");
    EXPECT_NE(out.find("for (let i = 1; i <= 10; i += 1) {"), std::string::npos);
}

TEST(Lua2JsTest, NumericForDownTo) {
    const std::string out = convert_ok("for i = 10, 1, -1 do\n  print(i)\nend\n");
    EXPECT_NE(out.find("for (let i = 10; i >= 1; i += -1) {"), std::string::npos);
}

TEST(Lua2JsTest, WhileLoop) {
    const std::string out = convert_ok("while hp > 0 do\n  hp = hp - 1\nend\n");
    EXPECT_NE(out.find("while (hp > 0) {"), std::string::npos);
}

TEST(Lua2JsTest, PairsFor) {
    const std::string out = convert_ok("for k, v in pairs(t) do\n  print(k)\nend\n");
    EXPECT_NE(out.find("for (const [k, v] of Object.entries(t)) {"), std::string::npos);
}

// ============================================================================
// 多返回值
// ============================================================================

TEST(Lua2JsTest, MultiReturnWrapped) {
    const std::string out = convert_ok("function pair()\n  return 1, 2\nend\n");
    EXPECT_NE(out.find("return [1, 2]"), std::string::npos);
}

// ============================================================================
// 端到端：转换后可在 QuickJS 中运行且语义一致
// ============================================================================

TEST(Lua2JsTest, ConvertedScriptRunsInQuickJS) {
    EXPECT_EQ(convert_and_run("function add(a, b)\n  return a + b\nend\n", "add",
                              {GryceEngineUtils::script::JSValueWrapper(2),
                               GryceEngineUtils::script::JSValueWrapper(3)}),
              "5");
}

TEST(Lua2JsTest, ConvertedIfElseRunsInQuickJS) {
    EXPECT_EQ(convert_and_run(
                  "function grade(v)\n"
                  "  if v >= 90 then\n"
                  "    return \"A\"\n"
                  "  elseif v >= 60 then\n"
                  "    return \"B\"\n"
                  "  else\n"
                  "    return \"C\"\n"
                  "  end\n"
                  "end\n",
                  "grade", {GryceEngineUtils::script::JSValueWrapper(85)}),
              "B");
}

TEST(Lua2JsTest, ConvertedForLoopRunsInQuickJS) {
    EXPECT_EQ(convert_and_run(
                  "function sum_to(n)\n"
                  "  local s = 0\n"
                  "  for i = 1, n do\n"
                  "    s = s + i\n"
                  "  end\n"
                  "  return s\n"
                  "end\n",
                  "sum_to", {GryceEngineUtils::script::JSValueWrapper(10)}),
              "55");
}

TEST(Lua2JsTest, ConvertedWhileRunsInQuickJS) {
    EXPECT_EQ(convert_and_run(
                  "function countdown(n)\n"
                  "  local c = 0\n"
                  "  while n > 0 do\n"
                  "    n = n - 1\n"
                  "    c = c + 1\n"
                  "  end\n"
                  "  return c\n"
                  "end\n",
                  "countdown", {GryceEngineUtils::script::JSValueWrapper(5)}),
              "5");
}
