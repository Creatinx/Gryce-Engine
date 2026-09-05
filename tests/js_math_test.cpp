#include <gtest/gtest.h>

#define _USE_MATH_DEFINES
#include <string>
#include <cmath>

#include "script/runtime/script_vm.h"
#include "script/bindings/math_bridge.h"

using namespace GryceEngineUtils::script;

// ============================================================================
// math.* 绑定测试
// ============================================================================

class JSMathTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(vm.init());
        register_math_bindings(vm.context());
    }

    void TearDown() override {
        vm.shutdown();
    }

    ScriptVM vm;
};

// 辅助宏：将 JS 返回值解析为 double 后与期望值比较
#define EXPECT_JS_EQ(expr, expected_val) do { \
    auto _r = vm.eval(expr); \
    EXPECT_TRUE(_r.success); \
    EXPECT_NEAR(std::stod(_r.result), static_cast<double>(expected_val), 0.001); \
} while(0)

#define EXPECT_JS_STR(expr, expected_str) do { \
    auto _r = vm.eval(expr); \
    EXPECT_TRUE(_r.success); \
    EXPECT_EQ(_r.result, expected_str); \
} while(0)

TEST_F(JSMathTest, MathObjectExists) {
    EXPECT_JS_STR("typeof math", "object");
}

TEST_F(JSMathTest, Lerp) {
    EXPECT_JS_EQ("math.lerp(0, 10, 0.5)", 5.0);
}

TEST_F(JSMathTest, LerpExtrapolate) {
    EXPECT_JS_EQ("math.lerp(0, 10, 1.5)", 15.0);
}

TEST_F(JSMathTest, InvLerp) {
    EXPECT_JS_EQ("math.inv_lerp(0, 10, 5)", 0.5);
}

TEST_F(JSMathTest, Remap) {
    EXPECT_JS_EQ("math.remap(5, 0, 10, 100, 200)", 150.0);
}

TEST_F(JSMathTest, Clamp) {
    EXPECT_JS_EQ("math.clamp(15, 0, 10)", 10.0);
}

TEST_F(JSMathTest, ClampLower) {
    EXPECT_JS_EQ("math.clamp(-5, 0, 10)", 0.0);
}

TEST_F(JSMathTest, Smoothstep) {
    EXPECT_JS_EQ("math.smoothstep(0, 1, 0.5)", 0.5);
}

TEST_F(JSMathTest, Smootherstep) {
    EXPECT_JS_EQ("math.smootherstep(0, 1, 0.5)", 0.5);
}

TEST_F(JSMathTest, SignPositive) {
    EXPECT_JS_STR("math.sign(42)", "1");
}

TEST_F(JSMathTest, SignNegative) {
    EXPECT_JS_STR("math.sign(-3.14)", "-1");
}

TEST_F(JSMathTest, SignZero) {
    EXPECT_JS_STR("math.sign(0)", "0");
}

TEST_F(JSMathTest, Wrap) {
    EXPECT_JS_EQ("math.wrap(370, 360)", 10.0);
}

TEST_F(JSMathTest, WrapNegative) {
    EXPECT_JS_EQ("math.wrap(-10, 360)", 350.0);
}

TEST_F(JSMathTest, Pingpong) {
    EXPECT_JS_EQ("math.pingpong(3, 5)", 3.0);
}

TEST_F(JSMathTest, PingpongAbove) {
    EXPECT_JS_EQ("math.pingpong(7, 5)", 3.0);
}

TEST_F(JSMathTest, MoveTowards) {
    EXPECT_JS_EQ("math.move_towards(0, 10, 3)", 3.0);
}

TEST_F(JSMathTest, MoveTowardsAtTarget) {
    EXPECT_JS_EQ("math.move_towards(8, 10, 3)", 10.0);
}

TEST_F(JSMathTest, Damp) {
    auto result = vm.eval("math.damp(0, 10, 2, 0.5)");
    EXPECT_TRUE(result.success);
    // damp uses exp, result should be approximately 10 * (1 - exp(-2*0.5)) = 10 * (1 - exp(-1)) ≈ 6.321
    EXPECT_NEAR(std::stod(result.result), 10.0 * (1.0 - std::exp(-1.0)), 0.001);
}

TEST_F(JSMathTest, AngleDelta) {
    EXPECT_JS_EQ("math.angle_delta(350, 10)", 20.0);
}

TEST_F(JSMathTest, AngleLerp) {
    EXPECT_JS_EQ("math.angle_lerp(0, 90, 0.5)", 45.0);
}

TEST_F(JSMathTest, Round) {
    EXPECT_JS_STR("math.round(3.7)", "4");
}

TEST_F(JSMathTest, Snap) {
    EXPECT_JS_EQ("math.snap(17, 5)", 15.0);
}

TEST_F(JSMathTest, Approximately) {
    EXPECT_JS_STR("math.approximately(0.1 + 0.2, 0.3)", "true");
}

// ============================================================================
// 缓动函数测试
// ============================================================================

TEST_F(JSMathTest, EaseLinear) {
    EXPECT_JS_EQ("math.ease_linear(0.5)", 0.5);
}

TEST_F(JSMathTest, EaseInQuad) {
    EXPECT_JS_EQ("math.ease_in_quad(0.5)", 0.25);
}

TEST_F(JSMathTest, EaseOutQuad) {
    EXPECT_JS_EQ("math.ease_out_quad(0.5)", 0.75);
}

TEST_F(JSMathTest, EaseInOutQuad) {
    EXPECT_JS_EQ("math.ease_in_out_quad(0.25)", 0.125);
}

TEST_F(JSMathTest, EaseInCubic) {
    EXPECT_JS_EQ("math.ease_in_cubic(0.5)", 0.125);
}

TEST_F(JSMathTest, EaseOutCubic) {
    EXPECT_JS_EQ("math.ease_out_cubic(0.5)", 0.875);
}

TEST_F(JSMathTest, EaseInOutCubic) {
    EXPECT_JS_EQ("math.ease_in_out_cubic(0.25)", 0.0625);
}

TEST_F(JSMathTest, EaseInSine) {
    auto result = vm.eval("math.ease_in_sine(0.5)");
    EXPECT_TRUE(result.success);
    double expected = 1.0 - std::cos(0.5 * M_PI / 2.0);
    EXPECT_NEAR(std::stod(result.result), expected, 0.001);
}

TEST_F(JSMathTest, EaseOutBack) {
    auto result = vm.eval("math.ease_out_back(0.5)");
    EXPECT_TRUE(result.success);
    EXPECT_GT(std::stod(result.result), 0.5); // overshoot
}

TEST_F(JSMathTest, EaseOutElastic) {
    auto result = vm.eval("math.ease_out_elastic(0.5)");
    EXPECT_TRUE(result.success);
    EXPECT_GT(std::stod(result.result), 0.5); // elastic overshoot
}

TEST_F(JSMathTest, EaseOutBounce) {
    auto result = vm.eval("math.ease_out_bounce(0.5)");
    EXPECT_TRUE(result.success);
    EXPECT_GT(std::stod(result.result), 0.5);
}

TEST_F(JSMathTest, AllEasingFunctionsExist) {
    auto result = vm.eval(R"(
        const names = [
            'ease_linear',
            'ease_in_quad', 'ease_out_quad', 'ease_in_out_quad',
            'ease_in_cubic', 'ease_out_cubic', 'ease_in_out_cubic',
            'ease_in_sine', 'ease_out_sine', 'ease_in_out_sine',
            'ease_in_expo', 'ease_out_expo', 'ease_in_out_expo',
            'ease_in_back', 'ease_out_back', 'ease_in_out_back',
            'ease_in_elastic', 'ease_out_elastic', 'ease_in_out_elastic',
            'ease_in_bounce', 'ease_out_bounce', 'ease_in_out_bounce'
        ];
        let allExist = true;
        for (const n of names) {
            if (typeof math[n] !== 'function') { allExist = false; break; }
        }
        allExist;
    )");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.result, "true");
}

TEST_F(JSMathTest, AllBaseFunctionsExist) {
    auto result = vm.eval(R"(
        const names = [
            'lerp', 'inv_lerp', 'remap', 'clamp',
            'smoothstep', 'smootherstep', 'sign', 'wrap',
            'pingpong', 'move_towards', 'damp',
            'angle_delta', 'angle_lerp', 'round', 'snap', 'approximately'
        ];
        let allExist = true;
        for (const n of names) {
            if (typeof math[n] !== 'function') { allExist = false; break; }
        }
        allExist;
    )");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.result, "true");
}