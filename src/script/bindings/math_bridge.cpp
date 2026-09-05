#include "math_bridge.h"

#define _USE_MATH_DEFINES
#include <cmath>
#include <algorithm>

#include <quickjs/quickjs-libc.h>

#include "utils/glog/glog_lib.h"

namespace GryceEngineUtils::script {

namespace {

// ============================================================================
// 基础插值/重映射
// ============================================================================

static JSValue js_math_lerp(JSContext* ctx, JSValueConst this_val,
                             int argc, JSValueConst* argv) {
    if (argc < 3) return JS_UNDEFINED;
    double a, b, t;
    JS_ToFloat64(ctx, &a, argv[0]);
    JS_ToFloat64(ctx, &b, argv[1]);
    JS_ToFloat64(ctx, &t, argv[2]);
    return JS_NewFloat64(ctx, a + (b - a) * t);
}

static JSValue js_math_inv_lerp(JSContext* ctx, JSValueConst this_val,
                                 int argc, JSValueConst* argv) {
    if (argc < 3) return JS_UNDEFINED;
    double a, b, v;
    JS_ToFloat64(ctx, &a, argv[0]);
    JS_ToFloat64(ctx, &b, argv[1]);
    JS_ToFloat64(ctx, &v, argv[2]);
    if (std::abs(b - a) < 1e-10) return JS_NewFloat64(ctx, 0);
    return JS_NewFloat64(ctx, (v - a) / (b - a));
}

static JSValue js_math_remap(JSContext* ctx, JSValueConst this_val,
                              int argc, JSValueConst* argv) {
    if (argc < 5) return JS_UNDEFINED;
    double v, a1, b1, a2, b2;
    JS_ToFloat64(ctx, &v, argv[0]);
    JS_ToFloat64(ctx, &a1, argv[1]);
    JS_ToFloat64(ctx, &b1, argv[2]);
    JS_ToFloat64(ctx, &a2, argv[3]);
    JS_ToFloat64(ctx, &b2, argv[4]);
    if (std::abs(b1 - a1) < 1e-10) return JS_NewFloat64(ctx, a2);
    return JS_NewFloat64(ctx, a2 + (v - a1) / (b1 - a1) * (b2 - a2));
}

static JSValue js_math_clamp(JSContext* ctx, JSValueConst this_val,
                              int argc, JSValueConst* argv) {
    if (argc < 3) return JS_UNDEFINED;
    double v, low, high;
    JS_ToFloat64(ctx, &v, argv[0]);
    JS_ToFloat64(ctx, &low, argv[1]);
    JS_ToFloat64(ctx, &high, argv[2]);
    return JS_NewFloat64(ctx, std::max(low, std::min(high, v)));
}

static JSValue js_math_smoothstep(JSContext* ctx, JSValueConst this_val,
                                   int argc, JSValueConst* argv) {
    if (argc < 3) return JS_UNDEFINED;
    double edge0, edge1, x;
    JS_ToFloat64(ctx, &edge0, argv[0]);
    JS_ToFloat64(ctx, &edge1, argv[1]);
    JS_ToFloat64(ctx, &x, argv[2]);
    double t = std::max(0.0, std::min(1.0, (x - edge0) / (edge1 - edge0)));
    return JS_NewFloat64(ctx, t * t * (3 - 2 * t));
}

static JSValue js_math_smootherstep(JSContext* ctx, JSValueConst this_val,
                                     int argc, JSValueConst* argv) {
    if (argc < 3) return JS_UNDEFINED;
    double edge0, edge1, x;
    JS_ToFloat64(ctx, &edge0, argv[0]);
    JS_ToFloat64(ctx, &edge1, argv[1]);
    JS_ToFloat64(ctx, &x, argv[2]);
    double t = std::max(0.0, std::min(1.0, (x - edge0) / (edge1 - edge0)));
    return JS_NewFloat64(ctx, t * t * t * (t * (t * 6 - 15) + 10));
}

static JSValue js_math_sign(JSContext* ctx, JSValueConst this_val,
                             int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    double v;
    JS_ToFloat64(ctx, &v, argv[0]);
    if (v > 0) return JS_NewFloat64(ctx, 1);
    if (v < 0) return JS_NewFloat64(ctx, -1);
    return JS_NewFloat64(ctx, 0);
}

static JSValue js_math_wrap(JSContext* ctx, JSValueConst this_val,
                             int argc, JSValueConst* argv) {
    if (argc < 2) return JS_UNDEFINED;
    double v, period;
    JS_ToFloat64(ctx, &v, argv[0]);
    JS_ToFloat64(ctx, &period, argv[1]);
    if (period <= 0) return JS_NewFloat64(ctx, 0);
    double r = std::fmod(v, period);
    if (r < 0) r += period;
    return JS_NewFloat64(ctx, r);
}

static JSValue js_math_pingpong(JSContext* ctx, JSValueConst this_val,
                                 int argc, JSValueConst* argv) {
    if (argc < 2) return JS_UNDEFINED;
    double v, length;
    JS_ToFloat64(ctx, &v, argv[0]);
    JS_ToFloat64(ctx, &length, argv[1]);
    if (length == 0) return JS_NewFloat64(ctx, 0);
    double t = std::fmod(v, length * 2.0);
    if (t < 0) t += length * 2.0;
    return JS_NewFloat64(ctx, length - std::abs(t - length));
}

static JSValue js_math_move_towards(JSContext* ctx, JSValueConst this_val,
                                     int argc, JSValueConst* argv) {
    if (argc < 3) return JS_UNDEFINED;
    double current, target, max_delta;
    JS_ToFloat64(ctx, &current, argv[0]);
    JS_ToFloat64(ctx, &target, argv[1]);
    JS_ToFloat64(ctx, &max_delta, argv[2]);
    double diff = target - current;
    if (std::abs(diff) <= max_delta) return JS_NewFloat64(ctx, target);
    return JS_NewFloat64(ctx, current + std::copysign(max_delta, diff));
}

static JSValue js_math_damp(JSContext* ctx, JSValueConst this_val,
                             int argc, JSValueConst* argv) {
    if (argc < 4) return JS_UNDEFINED;
    double current, target, smoothing, dt;
    JS_ToFloat64(ctx, &current, argv[0]);
    JS_ToFloat64(ctx, &target, argv[1]);
    JS_ToFloat64(ctx, &smoothing, argv[2]);
    JS_ToFloat64(ctx, &dt, argv[3]);
    if (smoothing <= 0) return JS_NewFloat64(ctx, target);
    double factor = 1.0 - std::exp(-smoothing * dt);
    return JS_NewFloat64(ctx, current + (target - current) * factor);
}

// ============================================================================
// 角度函数
// ============================================================================

static JSValue js_math_angle_delta(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv) {
    if (argc < 2) return JS_UNDEFINED;
    double a, b;
    JS_ToFloat64(ctx, &a, argv[0]);
    JS_ToFloat64(ctx, &b, argv[1]);
    double diff = std::fmod(b - a, 360.0);
    if (diff > 180.0) diff -= 360.0;
    else if (diff < -180.0) diff += 360.0;
    return JS_NewFloat64(ctx, diff);
}

static JSValue js_math_angle_lerp(JSContext* ctx, JSValueConst this_val,
                                   int argc, JSValueConst* argv) {
    if (argc < 3) return JS_UNDEFINED;
    double a, b, t;
    JS_ToFloat64(ctx, &a, argv[0]);
    JS_ToFloat64(ctx, &b, argv[1]);
    JS_ToFloat64(ctx, &t, argv[2]);
    double diff = std::fmod(b - a, 360.0);
    if (diff > 180.0) diff -= 360.0;
    else if (diff < -180.0) diff += 360.0;
    return JS_NewFloat64(ctx, a + diff * t);
}

static JSValue js_math_round(JSContext* ctx, JSValueConst this_val,
                              int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    double v;
    JS_ToFloat64(ctx, &v, argv[0]);
    return JS_NewFloat64(ctx, std::round(v));
}

static JSValue js_math_snap(JSContext* ctx, JSValueConst this_val,
                             int argc, JSValueConst* argv) {
    if (argc < 2) return JS_UNDEFINED;
    double v, step;
    JS_ToFloat64(ctx, &v, argv[0]);
    JS_ToFloat64(ctx, &step, argv[1]);
    if (step == 0) return JS_NewFloat64(ctx, v);
    return JS_NewFloat64(ctx, std::round(v / step) * step);
}

static JSValue js_math_approximately(JSContext* ctx, JSValueConst this_val,
                                      int argc, JSValueConst* argv) {
    if (argc < 2) return JS_UNDEFINED;
    double a, b;
    JS_ToFloat64(ctx, &a, argv[0]);
    JS_ToFloat64(ctx, &b, argv[1]);
    double eps = (argc >= 3) ? 0 : 1e-6;
    if (argc >= 3) JS_ToFloat64(ctx, &eps, argv[2]);
    return JS_NewBool(ctx, std::abs(a - b) < eps);
}

// ============================================================================
// 缓动函数
// ============================================================================

static JSValue js_ease_linear(JSContext* ctx, JSValueConst this_val,
                               int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    double t;
    JS_ToFloat64(ctx, &t, argv[0]);
    return JS_NewFloat64(ctx, std::max(0.0, std::min(1.0, t)));
}

// 通用缓动辅助
#define EASE_IN_QUAD(t)  ((t) * (t))
#define EASE_OUT_QUAD(t) ((t) * (2.0 - (t)))
#define EASE_IN_OUT_QUAD(t) ((t) < 0.5 ? 2.0 * (t) * (t) : -1.0 + (4.0 - 2.0 * (t)) * (t))

#define EASE_IN_CUBIC(t)  ((t) * (t) * (t))
#define EASE_OUT_CUBIC(t) (--(t), (t) * (t) * (t) + 1.0)
#define EASE_IN_OUT_CUBIC(t) ((t) < 0.5 ? 4.0 * (t) * (t) * (t) : (t - 1.0) * (2.0 * (t) - 2.0) * (2.0 * (t) - 2.0) + 1.0)

#define EASE_IN_SINE(t)  (1.0 - std::cos((t) * M_PI / 2.0))
#define EASE_OUT_SINE(t) (std::sin((t) * M_PI / 2.0))
#define EASE_IN_OUT_SINE(t) (-(std::cos(M_PI * (t)) - 1.0) / 2.0)

#define EASE_IN_EXPO(t)  ((t) <= 0.0 ? 0.0 : std::pow(2.0, 10.0 * (t) - 10.0))
#define EASE_OUT_EXPO(t) ((t) >= 1.0 ? 1.0 : 1.0 - std::pow(2.0, -10.0 * (t)))
#define EASE_IN_OUT_EXPO(t) \
    ((t) <= 0.0 ? 0.0 : (t) >= 1.0 ? 1.0 : \
     (t) < 0.5 ? std::pow(2.0, 20.0 * (t) - 10.0) / 2.0 : \
     (2.0 - std::pow(2.0, -20.0 * (t) + 10.0)) / 2.0)

#define EASE_IN_BACK(t)  (2.70158 * (t) * (t) * (t) - 1.70158 * (t) * (t))
#define EASE_OUT_BACK(t) (1.0 + 2.70158 * std::pow((t) - 1.0, 3.0) + 1.70158 * std::pow((t) - 1.0, 2.0))
#define EASE_IN_OUT_BACK(t) \
    ((t) < 0.5 ? std::pow(2.0 * (t), 2.0) * (7.189819 * (t) - 2.5949095) : \
     std::pow(2.0 * (t) - 2.0, 2.0) * (3.5949095 * ((t) - 1.0) - 2.5949095) + 1.0)

#define EASE_OUT_ELASTIC(t) \
    (std::pow(2.0, -10.0 * (t)) * std::sin(((t) * 10.0 - 0.75) * (2.0 * M_PI) / 3.0) + 1.0)

#define EASE_IN_ELASTIC(t) \
    (1.0 - std::pow(2.0, -10.0 * (t)) * std::cos(((t) * 10.0 - 0.75) * (2.0 * M_PI) / 3.0))

#define EASE_OUT_BOUNCE(t) \
    ((t) < 1.0 / 2.75 ? 7.5625 * (t) * (t) : \
     (t) < 2.0 / 2.75 ? 7.5625 * ((t) - 1.5 / 2.75) * ((t) - 1.5 / 2.75) + 0.75 : \
     (t) < 2.5 / 2.75 ? 7.5625 * ((t) - 2.25 / 2.75) * ((t) - 2.25 / 2.75) + 0.9375 : \
     7.5625 * ((t) - 2.625 / 2.75) * ((t) - 2.625 / 2.75) + 0.984375)

#define EASE_IN_BOUNCE(t) (1.0 - EASE_OUT_BOUNCE(1.0 - (t)))

// 生成缓动函数
#define MAKE_EASE_FUNC(name, expr) \
    static JSValue js_ease_##name(JSContext* ctx, JSValueConst this_val, \
                                   int argc, JSValueConst* argv) { \
        if (argc < 1) return JS_UNDEFINED; \
        double t; \
        JS_ToFloat64(ctx, &t, argv[0]); \
        t = std::max(0.0, std::min(1.0, t)); \
        return JS_NewFloat64(ctx, expr); \
    }

MAKE_EASE_FUNC(in_quad, EASE_IN_QUAD(t))
MAKE_EASE_FUNC(out_quad, EASE_OUT_QUAD(t))
MAKE_EASE_FUNC(in_out_quad, EASE_IN_OUT_QUAD(t))
MAKE_EASE_FUNC(in_cubic, EASE_IN_CUBIC(t))
MAKE_EASE_FUNC(out_cubic, EASE_OUT_CUBIC(t))
MAKE_EASE_FUNC(in_out_cubic, EASE_IN_OUT_CUBIC(t))
MAKE_EASE_FUNC(in_sine, EASE_IN_SINE(t))
MAKE_EASE_FUNC(out_sine, EASE_OUT_SINE(t))
MAKE_EASE_FUNC(in_out_sine, EASE_IN_OUT_SINE(t))
MAKE_EASE_FUNC(in_expo, EASE_IN_EXPO(t))
MAKE_EASE_FUNC(out_expo, EASE_OUT_EXPO(t))
MAKE_EASE_FUNC(in_out_expo, EASE_IN_OUT_EXPO(t))
MAKE_EASE_FUNC(in_back, EASE_IN_BACK(t))
MAKE_EASE_FUNC(out_back, EASE_OUT_BACK(t))
MAKE_EASE_FUNC(in_out_back, EASE_IN_OUT_BACK(t))
MAKE_EASE_FUNC(in_elastic, EASE_IN_ELASTIC(t))
MAKE_EASE_FUNC(out_elastic, EASE_OUT_ELASTIC(t))
MAKE_EASE_FUNC(in_out_elastic, (t) < 0.5 ? EASE_IN_ELASTIC((t) * 2.0) * 0.5 : EASE_OUT_ELASTIC((t) * 2.0 - 1.0) * 0.5 + 0.5)
MAKE_EASE_FUNC(in_bounce, EASE_IN_BOUNCE(t))
MAKE_EASE_FUNC(out_bounce, EASE_OUT_BOUNCE(t))
MAKE_EASE_FUNC(in_out_bounce, (t) < 0.5 ? EASE_IN_BOUNCE((t) * 2.0) * 0.5 : EASE_OUT_BOUNCE((t) * 2.0 - 1.0) * 0.5 + 0.5)

} // namespace

// ============================================================================
// 注册入口
// ============================================================================

void register_math_bindings(JSContext* ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue math = JS_NewObject(ctx);

    // 基础函数
    JS_SetPropertyStr(ctx, math, "lerp", JS_NewCFunction(ctx, js_math_lerp, "lerp", 3));
    JS_SetPropertyStr(ctx, math, "inv_lerp", JS_NewCFunction(ctx, js_math_inv_lerp, "inv_lerp", 3));
    JS_SetPropertyStr(ctx, math, "remap", JS_NewCFunction(ctx, js_math_remap, "remap", 5));
    JS_SetPropertyStr(ctx, math, "clamp", JS_NewCFunction(ctx, js_math_clamp, "clamp", 3));
    JS_SetPropertyStr(ctx, math, "smoothstep", JS_NewCFunction(ctx, js_math_smoothstep, "smoothstep", 3));
    JS_SetPropertyStr(ctx, math, "smootherstep", JS_NewCFunction(ctx, js_math_smootherstep, "smootherstep", 3));
    JS_SetPropertyStr(ctx, math, "sign", JS_NewCFunction(ctx, js_math_sign, "sign", 1));
    JS_SetPropertyStr(ctx, math, "wrap", JS_NewCFunction(ctx, js_math_wrap, "wrap", 2));
    JS_SetPropertyStr(ctx, math, "pingpong", JS_NewCFunction(ctx, js_math_pingpong, "pingpong", 2));
    JS_SetPropertyStr(ctx, math, "move_towards", JS_NewCFunction(ctx, js_math_move_towards, "move_towards", 3));
    JS_SetPropertyStr(ctx, math, "damp", JS_NewCFunction(ctx, js_math_damp, "damp", 4));
    JS_SetPropertyStr(ctx, math, "angle_delta", JS_NewCFunction(ctx, js_math_angle_delta, "angle_delta", 2));
    JS_SetPropertyStr(ctx, math, "angle_lerp", JS_NewCFunction(ctx, js_math_angle_lerp, "angle_lerp", 3));
    JS_SetPropertyStr(ctx, math, "round", JS_NewCFunction(ctx, js_math_round, "round", 1));
    JS_SetPropertyStr(ctx, math, "snap", JS_NewCFunction(ctx, js_math_snap, "snap", 2));
    JS_SetPropertyStr(ctx, math, "approximately", JS_NewCFunction(ctx, js_math_approximately, "approximately", 3));

    // 缓动函数
    JS_SetPropertyStr(ctx, math, "ease_linear", JS_NewCFunction(ctx, js_ease_linear, "ease_linear", 1));
    JS_SetPropertyStr(ctx, math, "ease_in_quad", JS_NewCFunction(ctx, js_ease_in_quad, "ease_in_quad", 1));
    JS_SetPropertyStr(ctx, math, "ease_out_quad", JS_NewCFunction(ctx, js_ease_out_quad, "ease_out_quad", 1));
    JS_SetPropertyStr(ctx, math, "ease_in_out_quad", JS_NewCFunction(ctx, js_ease_in_out_quad, "ease_in_out_quad", 1));
    JS_SetPropertyStr(ctx, math, "ease_in_cubic", JS_NewCFunction(ctx, js_ease_in_cubic, "ease_in_cubic", 1));
    JS_SetPropertyStr(ctx, math, "ease_out_cubic", JS_NewCFunction(ctx, js_ease_out_cubic, "ease_out_cubic", 1));
    JS_SetPropertyStr(ctx, math, "ease_in_out_cubic", JS_NewCFunction(ctx, js_ease_in_out_cubic, "ease_in_out_cubic", 1));
    JS_SetPropertyStr(ctx, math, "ease_in_sine", JS_NewCFunction(ctx, js_ease_in_sine, "ease_in_sine", 1));
    JS_SetPropertyStr(ctx, math, "ease_out_sine", JS_NewCFunction(ctx, js_ease_out_sine, "ease_out_sine", 1));
    JS_SetPropertyStr(ctx, math, "ease_in_out_sine", JS_NewCFunction(ctx, js_ease_in_out_sine, "ease_in_out_sine", 1));
    JS_SetPropertyStr(ctx, math, "ease_in_expo", JS_NewCFunction(ctx, js_ease_in_expo, "ease_in_expo", 1));
    JS_SetPropertyStr(ctx, math, "ease_out_expo", JS_NewCFunction(ctx, js_ease_out_expo, "ease_out_expo", 1));
    JS_SetPropertyStr(ctx, math, "ease_in_out_expo", JS_NewCFunction(ctx, js_ease_in_out_expo, "ease_in_out_expo", 1));
    JS_SetPropertyStr(ctx, math, "ease_in_back", JS_NewCFunction(ctx, js_ease_in_back, "ease_in_back", 1));
    JS_SetPropertyStr(ctx, math, "ease_out_back", JS_NewCFunction(ctx, js_ease_out_back, "ease_out_back", 1));
    JS_SetPropertyStr(ctx, math, "ease_in_out_back", JS_NewCFunction(ctx, js_ease_in_out_back, "ease_in_out_back", 1));
    JS_SetPropertyStr(ctx, math, "ease_in_elastic", JS_NewCFunction(ctx, js_ease_in_elastic, "ease_in_elastic", 1));
    JS_SetPropertyStr(ctx, math, "ease_out_elastic", JS_NewCFunction(ctx, js_ease_out_elastic, "ease_out_elastic", 1));
    JS_SetPropertyStr(ctx, math, "ease_in_out_elastic", JS_NewCFunction(ctx, js_ease_in_out_elastic, "ease_in_out_elastic", 1));
    JS_SetPropertyStr(ctx, math, "ease_in_bounce", JS_NewCFunction(ctx, js_ease_in_bounce, "ease_in_bounce", 1));
    JS_SetPropertyStr(ctx, math, "ease_out_bounce", JS_NewCFunction(ctx, js_ease_out_bounce, "ease_out_bounce", 1));
    JS_SetPropertyStr(ctx, math, "ease_in_out_bounce", JS_NewCFunction(ctx, js_ease_in_out_bounce, "ease_in_out_bounce", 1));

    // 注册到全局
    JS_SetPropertyStr(ctx, global, "math", math);
    JS_FreeValue(ctx, global);

    GLOG_INFO("ScriptEngine: registered math.* bindings ({}+ functions)", 38);
}

} // namespace GryceEngineUtils::script