#include "big_bridge.h"

#include <string>

#include "script/big_number.h"
#include "utils/glog/glog_lib.h"

namespace GryceEngineUtils::script {

using gryce_engine::script::BigInt;
using gryce_engine::script::BigDecimal;

namespace {

// ============================================================================
// BigInt 函数
// ============================================================================

// big.int(s) — 验证并规范化 BigInt 字符串
static JSValue js_big_int(JSContext* ctx, JSValueConst this_val,
                           int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "big.int: string argument required");
    const char* s = JS_ToCString(ctx, argv[0]);
    if (!s) return JS_ThrowTypeError(ctx, "big.int: argument must be a string");

    std::string err;
    BigInt v = BigInt::from_string(s, &err);
    JS_FreeCString(ctx, s);
    if (!err.empty()) return JS_ThrowTypeError(ctx, "%s", err.c_str());

    return JS_NewString(ctx, v.to_string().c_str());
}

// big.int_add(a, b)
static JSValue js_big_int_add(JSContext* ctx, JSValueConst this_val,
                               int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "big.int_add: two arguments required");
    const char* sa = JS_ToCString(ctx, argv[0]);
    const char* sb = JS_ToCString(ctx, argv[1]);
    if (!sa || !sb) { JS_FreeCString(ctx, sa); JS_FreeCString(ctx, sb);
        return JS_ThrowTypeError(ctx, "big.int_add: arguments must be strings"); }

    BigInt a = BigInt::from_string(sa);
    BigInt b = BigInt::from_string(sb);
    JS_FreeCString(ctx, sa);
    JS_FreeCString(ctx, sb);
    return JS_NewString(ctx, a.add(b).to_string().c_str());
}

// big.int_sub(a, b)
static JSValue js_big_int_sub(JSContext* ctx, JSValueConst this_val,
                               int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "big.int_sub: two arguments required");
    const char* sa = JS_ToCString(ctx, argv[0]);
    const char* sb = JS_ToCString(ctx, argv[1]);
    BigInt a = BigInt::from_string(sa);
    BigInt b = BigInt::from_string(sb);
    JS_FreeCString(ctx, sa);
    JS_FreeCString(ctx, sb);
    return JS_NewString(ctx, a.sub(b).to_string().c_str());
}

// big.int_mul(a, b)
static JSValue js_big_int_mul(JSContext* ctx, JSValueConst this_val,
                               int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "big.int_mul: two arguments required");
    const char* sa = JS_ToCString(ctx, argv[0]);
    const char* sb = JS_ToCString(ctx, argv[1]);
    BigInt a = BigInt::from_string(sa);
    BigInt b = BigInt::from_string(sb);
    JS_FreeCString(ctx, sa);
    JS_FreeCString(ctx, sb);
    return JS_NewString(ctx, a.mul(b).to_string().c_str());
}

// big.int_div(a, b)
static JSValue js_big_int_div(JSContext* ctx, JSValueConst this_val,
                               int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "big.int_div: two arguments required");
    const char* sa = JS_ToCString(ctx, argv[0]);
    const char* sb = JS_ToCString(ctx, argv[1]);
    BigInt a = BigInt::from_string(sa);
    BigInt b = BigInt::from_string(sb);
    JS_FreeCString(ctx, sa);
    JS_FreeCString(ctx, sb);
    if (b.is_zero()) return JS_ThrowTypeError(ctx, "big.int_div: division by zero");
    return JS_NewString(ctx, a.div(b).to_string().c_str());
}

// big.int_mod(a, b)
static JSValue js_big_int_mod(JSContext* ctx, JSValueConst this_val,
                               int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "big.int_mod: two arguments required");
    const char* sa = JS_ToCString(ctx, argv[0]);
    const char* sb = JS_ToCString(ctx, argv[1]);
    BigInt a = BigInt::from_string(sa);
    BigInt b = BigInt::from_string(sb);
    JS_FreeCString(ctx, sa);
    JS_FreeCString(ctx, sb);
    if (b.is_zero()) return JS_ThrowTypeError(ctx, "big.int_mod: division by zero");
    return JS_NewString(ctx, a.mod(b).to_string().c_str());
}

// big.int_pow(a, n)
static JSValue js_big_int_pow(JSContext* ctx, JSValueConst this_val,
                               int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "big.int_pow: two arguments required");
    const char* sa = JS_ToCString(ctx, argv[0]);
    BigInt a = BigInt::from_string(sa);
    JS_FreeCString(ctx, sa);
    int32_t exp;
    JS_ToInt32(ctx, &exp, argv[1]);
    if (exp < 0) return JS_ThrowTypeError(ctx, "big.int_pow: negative exponent not supported");
    return JS_NewString(ctx, a.pow(static_cast<int>(exp)).to_string().c_str());
}

// big.int_neg(a)
static JSValue js_big_int_neg(JSContext* ctx, JSValueConst this_val,
                               int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "big.int_neg: one argument required");
    const char* sa = JS_ToCString(ctx, argv[0]);
    BigInt a = BigInt::from_string(sa);
    JS_FreeCString(ctx, sa);
    return JS_NewString(ctx, a.neg().to_string().c_str());
}

// big.int_abs(a)
static JSValue js_big_int_abs(JSContext* ctx, JSValueConst this_val,
                               int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "big.int_abs: one argument required");
    const char* sa = JS_ToCString(ctx, argv[0]);
    BigInt a = BigInt::from_string(sa);
    JS_FreeCString(ctx, sa);
    return JS_NewString(ctx, a.abs().to_string().c_str());
}

// big.int_compare(a, b)
static JSValue js_big_int_compare(JSContext* ctx, JSValueConst this_val,
                                   int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "big.int_compare: two arguments required");
    const char* sa = JS_ToCString(ctx, argv[0]);
    const char* sb = JS_ToCString(ctx, argv[1]);
    BigInt a = BigInt::from_string(sa);
    BigInt b = BigInt::from_string(sb);
    JS_FreeCString(ctx, sa);
    JS_FreeCString(ctx, sb);
    return JS_NewInt32(ctx, a.compare(b));
}

// big.int_sign(a)
static JSValue js_big_int_sign(JSContext* ctx, JSValueConst this_val,
                                int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "big.int_sign: one argument required");
    const char* sa = JS_ToCString(ctx, argv[0]);
    BigInt a = BigInt::from_string(sa);
    JS_FreeCString(ctx, sa);
    return JS_NewInt32(ctx, a.sign());
}

// ============================================================================
// BigDecimal 函数
// ============================================================================

// big.decimal(s) — 验证并规范化 BigDecimal 字符串
static JSValue js_big_decimal(JSContext* ctx, JSValueConst this_val,
                               int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "big.decimal: string argument required");
    const char* s = JS_ToCString(ctx, argv[0]);
    if (!s) return JS_ThrowTypeError(ctx, "big.decimal: argument must be a string");

    std::string err;
    BigDecimal v = BigDecimal::from_string(s, &err);
    JS_FreeCString(ctx, s);
    if (!err.empty()) return JS_ThrowTypeError(ctx, "%s", err.c_str());

    return JS_NewString(ctx, v.to_string().c_str());
}

// big.decimal_add(a, b)
static JSValue js_big_decimal_add(JSContext* ctx, JSValueConst this_val,
                                   int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "big.decimal_add: two arguments required");
    const char* sa = JS_ToCString(ctx, argv[0]);
    const char* sb = JS_ToCString(ctx, argv[1]);
    BigDecimal a = BigDecimal::from_string(sa);
    BigDecimal b = BigDecimal::from_string(sb);
    JS_FreeCString(ctx, sa);
    JS_FreeCString(ctx, sb);
    return JS_NewString(ctx, a.add(b).to_string().c_str());
}

// big.decimal_sub(a, b)
static JSValue js_big_decimal_sub(JSContext* ctx, JSValueConst this_val,
                                   int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "big.decimal_sub: two arguments required");
    const char* sa = JS_ToCString(ctx, argv[0]);
    const char* sb = JS_ToCString(ctx, argv[1]);
    BigDecimal a = BigDecimal::from_string(sa);
    BigDecimal b = BigDecimal::from_string(sb);
    JS_FreeCString(ctx, sa);
    JS_FreeCString(ctx, sb);
    return JS_NewString(ctx, a.sub(b).to_string().c_str());
}

// big.decimal_mul(a, b)
static JSValue js_big_decimal_mul(JSContext* ctx, JSValueConst this_val,
                                   int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "big.decimal_mul: two arguments required");
    const char* sa = JS_ToCString(ctx, argv[0]);
    const char* sb = JS_ToCString(ctx, argv[1]);
    BigDecimal a = BigDecimal::from_string(sa);
    BigDecimal b = BigDecimal::from_string(sb);
    JS_FreeCString(ctx, sa);
    JS_FreeCString(ctx, sb);
    return JS_NewString(ctx, a.mul(b).to_string().c_str());
}

// big.decimal_div(a, b, precision)
static JSValue js_big_decimal_div(JSContext* ctx, JSValueConst this_val,
                                   int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "big.decimal_div: at least two arguments required");
    const char* sa = JS_ToCString(ctx, argv[0]);
    const char* sb = JS_ToCString(ctx, argv[1]);
    BigDecimal a = BigDecimal::from_string(sa);
    BigDecimal b = BigDecimal::from_string(sb);
    JS_FreeCString(ctx, sa);
    JS_FreeCString(ctx, sb);
    if (b.is_zero()) return JS_ThrowTypeError(ctx, "big.decimal_div: division by zero");
    int precision = 16;
    if (argc >= 3) {
        double d; JS_ToFloat64(ctx, &d, argv[2]);
        precision = static_cast<int>(d);
    }
    return JS_NewString(ctx, a.div(b, precision).to_string().c_str());
}

// big.decimal_compare(a, b)
static JSValue js_big_decimal_compare(JSContext* ctx, JSValueConst this_val,
                                       int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "big.decimal_compare: two arguments required");
    const char* sa = JS_ToCString(ctx, argv[0]);
    const char* sb = JS_ToCString(ctx, argv[1]);
    BigDecimal a = BigDecimal::from_string(sa);
    BigDecimal b = BigDecimal::from_string(sb);
    JS_FreeCString(ctx, sa);
    JS_FreeCString(ctx, sb);
    return JS_NewInt32(ctx, a.compare(b));
}

// big.decimal_floor(a)
static JSValue js_big_decimal_floor(JSContext* ctx, JSValueConst this_val,
                                     int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "big.decimal_floor: one argument required");
    const char* sa = JS_ToCString(ctx, argv[0]);
    BigDecimal a = BigDecimal::from_string(sa);
    JS_FreeCString(ctx, sa);
    return JS_NewString(ctx, a.floor().to_string().c_str());
}

// big.decimal_ceil(a)
static JSValue js_big_decimal_ceil(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "big.decimal_ceil: one argument required");
    const char* sa = JS_ToCString(ctx, argv[0]);
    BigDecimal a = BigDecimal::from_string(sa);
    JS_FreeCString(ctx, sa);
    return JS_NewString(ctx, a.ceil().to_string().c_str());
}

// big.decimal_round(a)
static JSValue js_big_decimal_round(JSContext* ctx, JSValueConst this_val,
                                     int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "big.decimal_round: one argument required");
    const char* sa = JS_ToCString(ctx, argv[0]);
    BigDecimal a = BigDecimal::from_string(sa);
    JS_FreeCString(ctx, sa);
    return JS_NewString(ctx, a.round().to_string().c_str());
}

// big.decimal_neg(a)
static JSValue js_big_decimal_neg(JSContext* ctx, JSValueConst this_val,
                                   int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "big.decimal_neg: one argument required");
    const char* sa = JS_ToCString(ctx, argv[0]);
    BigDecimal a = BigDecimal::from_string(sa);
    JS_FreeCString(ctx, sa);
    return JS_NewString(ctx, a.neg().to_string().c_str());
}

// big.decimal_abs(a)
static JSValue js_big_decimal_abs(JSContext* ctx, JSValueConst this_val,
                                   int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "big.decimal_abs: one argument required");
    const char* sa = JS_ToCString(ctx, argv[0]);
    BigDecimal a = BigDecimal::from_string(sa);
    JS_FreeCString(ctx, sa);
    return JS_NewString(ctx, a.abs().to_string().c_str());
}

// big.decimal_sign(a)
static JSValue js_big_decimal_sign(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "big.decimal_sign: one argument required");
    const char* sa = JS_ToCString(ctx, argv[0]);
    BigDecimal a = BigDecimal::from_string(sa);
    JS_FreeCString(ctx, sa);
    return JS_NewInt32(ctx, a.sign());
}

// ============================================================================
// big.decimal_pow(a, n, precision)
// ============================================================================
static JSValue js_big_decimal_pow(JSContext* ctx, JSValueConst this_val,
                                   int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "big.decimal_pow: at least two arguments required");
    const char* sa = JS_ToCString(ctx, argv[0]);
    BigDecimal a = BigDecimal::from_string(sa);
    JS_FreeCString(ctx, sa);
    int32_t exp;
    JS_ToInt32(ctx, &exp, argv[1]);
    int precision = 16;
    if (argc >= 3) {
        double d; JS_ToFloat64(ctx, &d, argv[2]);
        precision = static_cast<int>(d);
    }
    return JS_NewString(ctx, a.pow(static_cast<int>(exp), precision).to_string().c_str());
}

// big.decimal_sqrt(a, precision)
static JSValue js_big_decimal_sqrt(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "big.decimal_sqrt: at least one argument required");
    const char* sa = JS_ToCString(ctx, argv[0]);
    BigDecimal a = BigDecimal::from_string(sa);
    JS_FreeCString(ctx, sa);
    if (a.sign() < 0) return JS_ThrowTypeError(ctx, "big.decimal_sqrt: negative argument");
    int precision = 16;
    if (argc >= 2) {
        double d; JS_ToFloat64(ctx, &d, argv[1]);
        precision = static_cast<int>(d);
    }
    return JS_NewString(ctx, a.sqrt(precision).to_string().c_str());
}

// ============================================================================
// 注册函数
// ============================================================================

} // namespace

void register_big_bindings(JSContext* ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue big = JS_NewObject(ctx);

    // BigInt 函数
    JS_SetPropertyStr(ctx, big, "int", JS_NewCFunction(ctx, js_big_int, "int", 1));
    JS_SetPropertyStr(ctx, big, "int_add", JS_NewCFunction(ctx, js_big_int_add, "int_add", 2));
    JS_SetPropertyStr(ctx, big, "int_sub", JS_NewCFunction(ctx, js_big_int_sub, "int_sub", 2));
    JS_SetPropertyStr(ctx, big, "int_mul", JS_NewCFunction(ctx, js_big_int_mul, "int_mul", 2));
    JS_SetPropertyStr(ctx, big, "int_div", JS_NewCFunction(ctx, js_big_int_div, "int_div", 2));
    JS_SetPropertyStr(ctx, big, "int_mod", JS_NewCFunction(ctx, js_big_int_mod, "int_mod", 2));
    JS_SetPropertyStr(ctx, big, "int_pow", JS_NewCFunction(ctx, js_big_int_pow, "int_pow", 2));
    JS_SetPropertyStr(ctx, big, "int_neg", JS_NewCFunction(ctx, js_big_int_neg, "int_neg", 1));
    JS_SetPropertyStr(ctx, big, "int_abs", JS_NewCFunction(ctx, js_big_int_abs, "int_abs", 1));
    JS_SetPropertyStr(ctx, big, "int_compare", JS_NewCFunction(ctx, js_big_int_compare, "int_compare", 2));
    JS_SetPropertyStr(ctx, big, "int_sign", JS_NewCFunction(ctx, js_big_int_sign, "int_sign", 1));

    // BigDecimal 函数
    JS_SetPropertyStr(ctx, big, "decimal", JS_NewCFunction(ctx, js_big_decimal, "decimal", 1));
    JS_SetPropertyStr(ctx, big, "decimal_add", JS_NewCFunction(ctx, js_big_decimal_add, "decimal_add", 2));
    JS_SetPropertyStr(ctx, big, "decimal_sub", JS_NewCFunction(ctx, js_big_decimal_sub, "decimal_sub", 2));
    JS_SetPropertyStr(ctx, big, "decimal_mul", JS_NewCFunction(ctx, js_big_decimal_mul, "decimal_mul", 2));
    JS_SetPropertyStr(ctx, big, "decimal_div", JS_NewCFunction(ctx, js_big_decimal_div, "decimal_div", 3));
    JS_SetPropertyStr(ctx, big, "decimal_pow", JS_NewCFunction(ctx, js_big_decimal_pow, "decimal_pow", 3));
    JS_SetPropertyStr(ctx, big, "decimal_sqrt", JS_NewCFunction(ctx, js_big_decimal_sqrt, "decimal_sqrt", 2));
    JS_SetPropertyStr(ctx, big, "decimal_compare", JS_NewCFunction(ctx, js_big_decimal_compare, "decimal_compare", 2));
    JS_SetPropertyStr(ctx, big, "decimal_floor", JS_NewCFunction(ctx, js_big_decimal_floor, "decimal_floor", 1));
    JS_SetPropertyStr(ctx, big, "decimal_ceil", JS_NewCFunction(ctx, js_big_decimal_ceil, "decimal_ceil", 1));
    JS_SetPropertyStr(ctx, big, "decimal_round", JS_NewCFunction(ctx, js_big_decimal_round, "decimal_round", 1));
    JS_SetPropertyStr(ctx, big, "decimal_neg", JS_NewCFunction(ctx, js_big_decimal_neg, "decimal_neg", 1));
    JS_SetPropertyStr(ctx, big, "decimal_abs", JS_NewCFunction(ctx, js_big_decimal_abs, "decimal_abs", 1));
    JS_SetPropertyStr(ctx, big, "decimal_sign", JS_NewCFunction(ctx, js_big_decimal_sign, "decimal_sign", 1));

    JS_SetPropertyStr(ctx, global, "big", big);
    JS_FreeValue(ctx, global);

    GLOG_INFO("BigBridge: registered big.* module");
}

} // namespace GryceEngineUtils::script