#include <gtest/gtest.h>

#include <string>

#include "script/lua_runtime.h"

using namespace gryce_engine;

// GryceSRT 数学扩展与超高精度模块（math.* / big.*）测试。
class LuaMathTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto& rt = script::LuaRuntime::instance();
        ASSERT_TRUE(rt.init()) << "Lua runtime failed to initialize";
    }

    void TearDown() override {
        script::LuaRuntime::instance().shutdown();
    }

    // 运行 chunk，并把 _test_result（若设置）取回
    std::string run(const char* code, bool& ok, std::string& err) {
        ok = script::LuaRuntime::instance().run_string(code, &err);
        std::string out;
        if (ok) {
            script::LuaRuntime::instance().get_global_string("_test_result", out);
            script::LuaRuntime::instance().run_string("_test_result = nil", nullptr);
        }
        return out;
    }

    std::string run_ok(const char* code) {
        bool ok = false;
        std::string err;
        const std::string out = run(code, ok, err);
        EXPECT_TRUE(ok) << err;
        return out;
    }

};

TEST_F(LuaMathTest, MathExtensionsReturnExpectedValues) {
    run_ok("if math.lerp(0, 10, 0.5) ~= 5 then error('lerp') end");
    run_ok("if math.clamp(7, 0, 5) ~= 5 then error('clamp') end");
    run_ok("if math.remap(5, 0, 10, 100, 200) ~= 150 then error('remap') end");
    run_ok("if math.smoothstep(0, 1, 0.5) ~= 0.5 then error('smoothstep') end");
    run_ok("if math.sign(-3) ~= -1 then error('sign') end");
    run_ok("if math.round(2.6) ~= 3 then error('round') end");
    run_ok("if math.wrap(-1, 5) ~= 4 then error('wrap') end");
    run_ok("if math.pingpong(7, 4) ~= 1 then error('pingpong') end");
    run_ok("if math.move_towards(5, 10, 2) ~= 7 then error('move_towards') end");
    run_ok("if math.snap(3.7, 1) ~= 4 then error('snap') end");
    run_ok("if not math.approximately(0.1 + 0.2, 0.3) then error('approximately') end");
    run_ok("if math.ease_in_out_quad(0.5) ~= 0.5 then error('ease') end");
    run_ok("_test_result = tostring(math.angle_lerp(0, math.pi, 0.5))");
}

TEST_F(LuaMathTest, BigIntExactFibonacci) {
    const std::string out = run_ok(
        "local a = big.int(0)\n"
        "local b = big.int(1)\n"
        "for i = 1, 100 do\n"
        "  local c = a + b\n"
        "  a = b\n"
        "  b = c\n"
        "end\n"
        "_test_result = tostring(a)");
    // F(100) 精确值（double 早已溢出/失真）
    EXPECT_EQ(out, "354224848179261915075");
}

TEST_F(LuaMathTest, BigDecimalExactAddition) {
    const std::string out = run_ok(
        "local a = big.decimal('0.1')\n"
        "local b = big.decimal('0.2')\n"
        "_test_result = tostring(a + b)");
    EXPECT_EQ(out, "0.3");
}

TEST_F(LuaMathTest, BigDecimalHighPrecisionDivision) {
    const std::string out = run_ok(
        "local a = big.decimal('1')\n"
        "local b = big.decimal('3')\n"
        "_test_result = tostring(a / b)");
    EXPECT_EQ(out, "0.33333333333333333333333333333333");
}

TEST_F(LuaMathTest, BigDecimalSqrtPrecision) {
    const std::string out = run_ok(
        "local two = big.decimal('2', 50)\n"
        "_test_result = tostring(two:sqrt(40))");
    EXPECT_EQ(out, "1.4142135623730950488016887242096980785697");
}

TEST_F(LuaMathTest, BigDecimalPowAndConstants) {
    const std::string p = run_ok("_test_result = tostring(big.pi(50))");
    EXPECT_EQ(p, "3.14159265358979323846264338327950288419716939937511");

    const std::string e = run_ok("_test_result = tostring(big.e(30))");
    EXPECT_EQ(e, "2.718281828459045235360287471353");

    const std::string sq = run_ok("_test_result = tostring(big.pow(big.decimal('1.01'), 365, 12))");
    EXPECT_EQ(sq, "37.783434332887");
}

TEST_F(LuaMathTest, BigDecimalComparisonAndRounding) {
    run_ok(
        "local a = big.decimal('1.5')\n"
        "local b = big.decimal('2.5')\n"
        "if not (a < b) then error('lt') end\n"
        "if not (a <= a) then error('le') end\n"
        "if not (a ~= b) then error('neq') end\n"
        "if tostring(a:floor()) ~= '1' then error('floor') end\n"
        "if tostring(a:ceil()) ~= '2' then error('ceil') end\n"
        "if tostring(a:round()) ~= '2' then error('round') end\n"
        "if tostring(-a) ~= '-1.5' then error('unm') end\n"
        "if tostring(big.int('3.9')) ~= '3' then error('int trunc') end\n"
        "if tostring(big.int('-3.9')) ~= '-3' then error('int trunc neg') end");
}

TEST_F(LuaMathTest, BigDecimalDivisionByZeroIsReported) {
    bool ok = false;
    std::string err;
    run("local a = big.decimal('1') / big.decimal('0')", ok, err);
    EXPECT_FALSE(ok);
    EXPECT_NE(err.find("division by zero"), std::string::npos) << err;
}
