#include <gtest/gtest.h>

#include <quickjs/quickjs.h>

#include "script/bindings/big_bridge.h"
#include "script/runtime/script_vm.h"

namespace {

using GryceEngineUtils::script::ScriptVM;
using GryceEngineUtils::script::register_big_bindings;

// ============================================================================
// big.* 绑定测试
// ============================================================================

class JSBigBridgeTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(vm.init());
        register_big_bindings(vm.context());
    }

    void TearDown() override {
        vm.shutdown();
    }

    ScriptVM vm;
};

#define EXPECT_JS_STR(expr, expected_str) do { \
    auto _r = vm.eval(expr); \
    EXPECT_TRUE(_r.success) << "Error: " << _r.error_msg; \
    EXPECT_EQ(_r.result, expected_str); \
} while(0)

TEST_F(JSBigBridgeTest, BigObjectExists) {
    EXPECT_JS_STR("typeof big", "object");
}

TEST_F(JSBigBridgeTest, BigIntCreate) {
    EXPECT_JS_STR("big.int('12345678901234567890')", "12345678901234567890");
}

TEST_F(JSBigBridgeTest, BigIntAdd) {
    EXPECT_JS_STR("big.int_add('100', '200')", "300");
}

TEST_F(JSBigBridgeTest, BigIntAddLarge) {
    EXPECT_JS_STR("big.int_add('9999999999999999999', '1')", "10000000000000000000");
}

TEST_F(JSBigBridgeTest, BigIntSub) {
    EXPECT_JS_STR("big.int_sub('200', '100')", "100");
}

TEST_F(JSBigBridgeTest, BigIntSubNegative) {
    EXPECT_JS_STR("big.int_sub('100', '200')", "-100");
}

TEST_F(JSBigBridgeTest, BigIntMul) {
    EXPECT_JS_STR("big.int_mul('123456789', '987654321')", "121932631112635269");
}

TEST_F(JSBigBridgeTest, BigIntDiv) {
    EXPECT_JS_STR("big.int_div('100', '3')", "33");
}

TEST_F(JSBigBridgeTest, BigIntMod) {
    EXPECT_JS_STR("big.int_mod('100', '3')", "1");
}

TEST_F(JSBigBridgeTest, BigIntPow) {
    EXPECT_JS_STR("big.int_pow('10', 3)", "1000");
}

TEST_F(JSBigBridgeTest, BigIntNeg) {
    EXPECT_JS_STR("big.int_neg('100')", "-100");
}

TEST_F(JSBigBridgeTest, BigIntNegNegative) {
    EXPECT_JS_STR("big.int_neg('-100')", "100");
}

TEST_F(JSBigBridgeTest, BigIntAbs) {
    EXPECT_JS_STR("big.int_abs('-100')", "100");
}

TEST_F(JSBigBridgeTest, BigIntCompare) {
    EXPECT_JS_STR("big.int_compare('100', '200')", "-1");
    EXPECT_JS_STR("big.int_compare('200', '100')", "1");
    EXPECT_JS_STR("big.int_compare('100', '100')", "0");
}

TEST_F(JSBigBridgeTest, BigIntSign) {
    EXPECT_JS_STR("big.int_sign('100')", "1");
    EXPECT_JS_STR("big.int_sign('-100')", "-1");
    EXPECT_JS_STR("big.int_sign('0')", "0");
}

TEST_F(JSBigBridgeTest, BigIntZero) {
    EXPECT_JS_STR("big.int('0')", "0");
}

TEST_F(JSBigBridgeTest, BigIntNegative) {
    EXPECT_JS_STR("big.int('-12345')", "-12345");
}

// ============================================================================
// BigDecimal 测试
// ============================================================================

TEST_F(JSBigBridgeTest, BigDecimalCreate) {
    EXPECT_JS_STR("big.decimal('3.14159265358979323846')", "3.14159265358979323846");
}

TEST_F(JSBigBridgeTest, BigDecimalAdd) {
    EXPECT_JS_STR("big.decimal_add('1.5', '2.5')", "4");
}

TEST_F(JSBigBridgeTest, BigDecimalSub) {
    EXPECT_JS_STR("big.decimal_sub('5.0', '3.0')", "2");
}

TEST_F(JSBigBridgeTest, BigDecimalMul) {
    EXPECT_JS_STR("big.decimal_mul('1.5', '2.0')", "3");
}

TEST_F(JSBigBridgeTest, BigDecimalDiv) {
    EXPECT_JS_STR("big.decimal_div('10', '3', 4)", "3.3333");
}

TEST_F(JSBigBridgeTest, BigDecimalCompare) {
    EXPECT_JS_STR("big.decimal_compare('3.14', '3.14')", "0");
    EXPECT_JS_STR("big.decimal_compare('3.14', '2.71')", "1");
    EXPECT_JS_STR("big.decimal_compare('2.71', '3.14')", "-1");
}

TEST_F(JSBigBridgeTest, BigDecimalFloor) {
    EXPECT_JS_STR("big.decimal_floor('3.14')", "3");
    EXPECT_JS_STR("big.decimal_floor('-3.14')", "-4");
}

TEST_F(JSBigBridgeTest, BigDecimalCeil) {
    EXPECT_JS_STR("big.decimal_ceil('3.14')", "4");
    EXPECT_JS_STR("big.decimal_ceil('-3.14')", "-3");
}

TEST_F(JSBigBridgeTest, BigDecimalRound) {
    EXPECT_JS_STR("big.decimal_round('3.14')", "3");
    EXPECT_JS_STR("big.decimal_round('3.5')", "4");
    EXPECT_JS_STR("big.decimal_round('-3.5')", "-4");
}

TEST_F(JSBigBridgeTest, BigDecimalNeg) {
    EXPECT_JS_STR("big.decimal_neg('3.14')", "-3.14");
}

TEST_F(JSBigBridgeTest, BigDecimalAbs) {
    EXPECT_JS_STR("big.decimal_abs('-3.14')", "3.14");
}

TEST_F(JSBigBridgeTest, BigDecimalSign) {
    EXPECT_JS_STR("big.decimal_sign('3.14')", "1");
    EXPECT_JS_STR("big.decimal_sign('-3.14')", "-1");
    EXPECT_JS_STR("big.decimal_sign('0')", "0");
}

TEST_F(JSBigBridgeTest, BigDecimalZero) {
    EXPECT_JS_STR("big.decimal('0')", "0");
}

TEST_F(JSBigBridgeTest, BigDecimalNegative) {
    EXPECT_JS_STR("big.decimal('-3.14')", "-3.14");
}

TEST_F(JSBigBridgeTest, BigDecimalPow) {
    EXPECT_JS_STR("big.decimal_pow('2', 3, 4)", "8");
}

TEST_F(JSBigBridgeTest, BigDecimalSqrt) {
    EXPECT_JS_STR("big.decimal_sqrt('9', 4)", "3");
}

TEST_F(JSBigBridgeTest, BigDecimalSqrt2) {
    auto result = vm.eval("big.decimal_sqrt('2', 10)");
    EXPECT_TRUE(result.success);
    EXPECT_NEAR(std::stod(result.result), 1.4142135624, 0.0001);
}

} // namespace