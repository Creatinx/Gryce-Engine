#include "script/lua_math_bindings.h"

#include "script/big_number.h"

#include "utils/glog/glog_lib.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

#include <cmath>
#include <cstring>
#include <string>

namespace gryce_engine::script {

namespace {

constexpr double k_pi = 3.14159265358979323846;

// ===========================================================================
// math.* 扩展
// ===========================================================================
int l_math_lerp(lua_State* L) {
    const double a = luaL_checknumber(L, 1);
    const double b = luaL_checknumber(L, 2);
    const double t = luaL_checknumber(L, 3);
    lua_pushnumber(L, a + (b - a) * t);
    return 1;
}

int l_math_inv_lerp(lua_State* L) {
    const double a = luaL_checknumber(L, 1);
    const double b = luaL_checknumber(L, 2);
    const double v = luaL_checknumber(L, 3);
    lua_pushnumber(L, b == a ? 0.0 : (v - a) / (b - a));
    return 1;
}

int l_math_remap(lua_State* L) {
    const double v = luaL_checknumber(L, 1);
    const double a = luaL_checknumber(L, 2);
    const double b = luaL_checknumber(L, 3);
    const double c = luaL_checknumber(L, 4);
    const double d = luaL_checknumber(L, 5);
    const double t = b == a ? 0.0 : (v - a) / (b - a);
    lua_pushnumber(L, c + (d - c) * t);
    return 1;
}

int l_math_clamp(lua_State* L) {
    const double v = luaL_checknumber(L, 1);
    const double lo = luaL_checknumber(L, 2);
    const double hi = luaL_checknumber(L, 3);
    lua_pushnumber(L, v < lo ? lo : (v > hi ? hi : v));
    return 1;
}

int l_math_smoothstep(lua_State* L) {
    const double e0 = luaL_checknumber(L, 1);
    const double e1 = luaL_checknumber(L, 2);
    const double x = luaL_checknumber(L, 3);
    double t = (x - e0) / (e1 - e0);
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    lua_pushnumber(L, t * t * (3.0 - 2.0 * t));
    return 1;
}

int l_math_smootherstep(lua_State* L) {
    const double e0 = luaL_checknumber(L, 1);
    const double e1 = luaL_checknumber(L, 2);
    const double x = luaL_checknumber(L, 3);
    double t = (x - e0) / (e1 - e0);
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    lua_pushnumber(L, t * t * t * (t * (t * 6.0 - 15.0) + 10.0));
    return 1;
}

int l_math_sign(lua_State* L) {
    const double v = luaL_checknumber(L, 1);
    lua_pushnumber(L, v > 0.0 ? 1.0 : (v < 0.0 ? -1.0 : 0.0));
    return 1;
}

int l_math_wrap(lua_State* L) {
    const double v = luaL_checknumber(L, 1);
    const double len = luaL_checknumber(L, 2);
    if (len <= 0.0) {
        lua_pushnumber(L, 0.0);
        return 1;
    }
    double r = std::fmod(v, len);
    if (r < 0.0) r += len;
    lua_pushnumber(L, r);
    return 1;
}

int l_math_pingpong(lua_State* L) {
    const double t = luaL_checknumber(L, 1);
    const double len = luaL_checknumber(L, 2);
    if (len <= 0.0) {
        lua_pushnumber(L, 0.0);
        return 1;
    }
    double r = std::fmod(t, len * 2.0);
    if (r < 0.0) r += len * 2.0;
    lua_pushnumber(L, len - std::fabs(r - len));
    return 1;
}

int l_math_move_towards(lua_State* L) {
    double cur = luaL_checknumber(L, 1);
    const double target = luaL_checknumber(L, 2);
    const double max_delta = std::fabs(luaL_checknumber(L, 3));
    if (std::fabs(target - cur) <= max_delta) cur = target;
    else cur += (target > cur ? 1.0 : -1.0) * max_delta;
    lua_pushnumber(L, cur);
    return 1;
}

int l_math_damp(lua_State* L) {
    const double cur = luaL_checknumber(L, 1);
    const double target = luaL_checknumber(L, 2);
    const double lambda = luaL_checknumber(L, 3);
    const double dt = luaL_checknumber(L, 4);
    lua_pushnumber(L, target + (cur - target) * std::exp(-lambda * dt));
    return 1;
}

int l_math_angle_delta(lua_State* L) {
    const double a = luaL_checknumber(L, 1);
    const double b = luaL_checknumber(L, 2);
    double d = std::fmod(b - a, k_pi * 2.0);
    if (d > k_pi) d -= k_pi * 2.0;
    if (d < -k_pi) d += k_pi * 2.0;
    lua_pushnumber(L, d);
    return 1;
}

int l_math_angle_lerp(lua_State* L) {
    const double a = luaL_checknumber(L, 1);
    const double b = luaL_checknumber(L, 2);
    const double t = luaL_checknumber(L, 3);
    double d = std::fmod(b - a, k_pi * 2.0);
    if (d > k_pi) d -= k_pi * 2.0;
    if (d < -k_pi) d += k_pi * 2.0;
    lua_pushnumber(L, a + d * t);
    return 1;
}

int l_math_round(lua_State* L) {
    const double v = luaL_checknumber(L, 1);
    lua_pushnumber(L, std::floor(v + 0.5));
    return 1;
}

int l_math_snap(lua_State* L) {
    const double v = luaL_checknumber(L, 1);
    const double step = luaL_checknumber(L, 2);
    if (step == 0.0) {
        lua_pushnumber(L, v);
        return 1;
    }
    lua_pushnumber(L, std::floor(v / step + 0.5) * step);
    return 1;
}

int l_math_approximately(lua_State* L) {
    const double a = luaL_checknumber(L, 1);
    const double b = luaL_checknumber(L, 2);
    const double eps = luaL_optnumber(L, 3, 1e-6);
    lua_pushboolean(L, std::fabs(a - b) <= eps);
    return 1;
}

int l_math_ease_linear(lua_State* L) {
    lua_pushnumber(L, luaL_checknumber(L, 1));
    return 1;
}

int l_math_ease_in_quad(lua_State* L) {
    const double t = luaL_checknumber(L, 1);
    lua_pushnumber(L, t * t);
    return 1;
}

int l_math_ease_out_quad(lua_State* L) {
    const double t = luaL_checknumber(L, 1);
    lua_pushnumber(L, 1.0 - (1.0 - t) * (1.0 - t));
    return 1;
}

int l_math_ease_in_out_quad(lua_State* L) {
    const double t = luaL_checknumber(L, 1);
    lua_pushnumber(L, t < 0.5 ? 2.0 * t * t : 1.0 - std::pow(-2.0 * t + 2.0, 2.0) * 0.5);
    return 1;
}

int l_math_ease_in_out_cubic(lua_State* L) {
    const double t = luaL_checknumber(L, 1);
    lua_pushnumber(L, t < 0.5 ? 4.0 * t * t * t : 1.0 - std::pow(-2.0 * t + 2.0, 3.0) * 0.5);
    return 1;
}

int l_math_ease_in_out_sine(lua_State* L) {
    const double t = luaL_checknumber(L, 1);
    lua_pushnumber(L, -(std::cos(k_pi * t) - 1.0) * 0.5);
    return 1;
}

const luaL_Reg kMathExt[] = {
    {"lerp", l_math_lerp},
    {"inv_lerp", l_math_inv_lerp},
    {"remap", l_math_remap},
    {"clamp", l_math_clamp},
    {"smoothstep", l_math_smoothstep},
    {"smootherstep", l_math_smootherstep},
    {"sign", l_math_sign},
    {"wrap", l_math_wrap},
    {"pingpong", l_math_pingpong},
    {"move_towards", l_math_move_towards},
    {"damp", l_math_damp},
    {"angle_delta", l_math_angle_delta},
    {"angle_lerp", l_math_angle_lerp},
    {"round", l_math_round},
    {"snap", l_math_snap},
    {"approximately", l_math_approximately},
    {"ease_linear", l_math_ease_linear},
    {"ease_in_quad", l_math_ease_in_quad},
    {"ease_out_quad", l_math_ease_out_quad},
    {"ease_in_out_quad", l_math_ease_in_out_quad},
    {"ease_in_out_cubic", l_math_ease_in_out_cubic},
    {"ease_in_out_sine", l_math_ease_in_out_sine},
    {nullptr, nullptr}
};

// ===========================================================================
// big.* 超高精度数值模块
// ===========================================================================
const char* const k_big_metatable = "GryceBigDecimal";

int l_big_default_precision(lua_State* L) {
    lua_pushinteger(L, lua_tointeger(L, lua_upvalueindex(1)));
    return 1;
}

BigDecimal* check_big(lua_State* L, int idx) {
    return static_cast<BigDecimal*>(luaL_checkudata(L, idx, k_big_metatable));
}

// 把 number / string / userdata 统一转换为 BigDecimal
BigDecimal to_bigdecimal(lua_State* L, int idx, std::string* err = nullptr) {
    if (lua_isuserdata(L, idx)) {
        void* p = luaL_testudata(L, idx, k_big_metatable);
        if (p) return *static_cast<BigDecimal*>(p);
    }
    // 注意：lua_isnumber 对可转数字的字符串也返回 true，
    // 必须先按 Lua 类型区分，避免把 "0.1" 走 double 精度。
    if (lua_type(L, idx) == LUA_TNUMBER) {
        return BigDecimal::from_double(lua_tonumber(L, idx));
    }
    if (lua_isstring(L, idx)) {
        const char* s = lua_tostring(L, idx);
        return BigDecimal::from_string(s ? s : "", err);
    }
    if (err) *err = "expected number, string or big number";
    return BigDecimal();
}

void push_big(lua_State* L, const BigDecimal& v) {
    auto* bd = static_cast<BigDecimal*>(lua_newuserdatauv(L, sizeof(BigDecimal), 0));
    new (bd) BigDecimal(v);
    luaL_setmetatable(L, k_big_metatable);
}

int l_big_new(lua_State* L) {
    const int nargs = lua_gettop(L);
    std::string err;
    BigDecimal v = to_bigdecimal(L, 1, &err);
    if (!err.empty()) return luaL_argerror(L, 1, err.c_str());
    if (nargs >= 2) {
        const int precision = static_cast<int>(luaL_checkinteger(L, 2));
        v = v.set_precision(precision);
    } else {
        const int def = static_cast<int>(lua_tointeger(L, lua_upvalueindex(1)));
        v = v.set_precision(def);
    }
    push_big(L, v);
    return 1;
}

int l_big_int(lua_State* L) {
    std::string err;
    BigDecimal v = to_bigdecimal(L, 1, &err);
    if (!err.empty()) return luaL_argerror(L, 1, err.c_str());
    v = v.sign() < 0 ? v.ceil() : v.floor();
    push_big(L, v);
    return 1;
}

int l_big_is_big(lua_State* L) {
    if (lua_isuserdata(L, 1)) {
        lua_getmetatable(L, 1);
        luaL_getmetatable(L, k_big_metatable);
        const bool ok = lua_rawequal(L, -1, -2) == 1;
        lua_pop(L, 2);
        lua_pushboolean(L, ok);
        return 1;
    }
    lua_pushboolean(L, false);
    return 1;
}

int l_big_abs(lua_State* L) { push_big(L, to_bigdecimal(L, 1).abs()); return 1; }
int l_big_neg(lua_State* L) { push_big(L, to_bigdecimal(L, 1).neg()); return 1; }
int l_big_floor(lua_State* L) { push_big(L, to_bigdecimal(L, 1).floor()); return 1; }
int l_big_ceil(lua_State* L) { push_big(L, to_bigdecimal(L, 1).ceil()); return 1; }
int l_big_round(lua_State* L) { push_big(L, to_bigdecimal(L, 1).round()); return 1; }

int l_big_sqrt(lua_State* L) {
    BigDecimal v = to_bigdecimal(L, 1);
    const int def = static_cast<int>(lua_tointeger(L, lua_upvalueindex(1)));
    const int precision = static_cast<int>(luaL_optinteger(L, 2, def));
    push_big(L, v.sqrt(precision));
    return 1;
}

int l_big_pow(lua_State* L) {
    BigDecimal base = to_bigdecimal(L, 1);
    const int exp = static_cast<int>(luaL_checkinteger(L, 2));
    const int def = static_cast<int>(lua_tointeger(L, lua_upvalueindex(1)));
    const int precision = static_cast<int>(luaL_optinteger(L, 3, def));
    push_big(L, base.pow(exp, precision));
    return 1;
}

int l_big_cmp(lua_State* L) {
    const BigDecimal a = to_bigdecimal(L, 1);
    const BigDecimal b = to_bigdecimal(L, 2);
    lua_pushinteger(L, a.compare(b));
    return 1;
}

int l_big_to_float(lua_State* L) {
    const BigDecimal v = to_bigdecimal(L, 1);
    lua_pushnumber(L, v.to_double());
    return 1;
}

int l_big_to_string(lua_State* L) {
    BigDecimal v = to_bigdecimal(L, 1);
    if (lua_gettop(L) >= 2) {
        const int places = static_cast<int>(luaL_checkinteger(L, 2));
        v = v.set_precision(places);
    }
    lua_pushstring(L, v.to_string().c_str());
    return 1;
}

int l_big_pi(lua_State* L) {
    static const char* k_pi_high =
        "3.141592653589793238462643383279502884197169399375105820974944592307816406286208998628034825342117067982148086513282306647";
    const int def = static_cast<int>(lua_tointeger(L, lua_upvalueindex(1)));
    const int precision = static_cast<int>(luaL_optinteger(L, 1, def));
    push_big(L, BigDecimal::from_string(k_pi_high).set_precision(precision));
    return 1;
}

int l_big_e(lua_State* L) {
    static const char* k_e_high =
        "2.718281828459045235360287471352662497757247093699959574966967627724076630353547594571382178525166427427466391932003059921";
    const int def = static_cast<int>(lua_tointeger(L, lua_upvalueindex(1)));
    const int precision = static_cast<int>(luaL_optinteger(L, 1, def));
    push_big(L, BigDecimal::from_string(k_e_high).set_precision(precision));
    return 1;
}

// --- 算术 metamethods ---
int l_big_add(lua_State* L) {
    push_big(L, to_bigdecimal(L, 1).add(to_bigdecimal(L, 2)));
    return 1;
}
int l_big_sub(lua_State* L) {
    push_big(L, to_bigdecimal(L, 1).sub(to_bigdecimal(L, 2)));
    return 1;
}
int l_big_mul(lua_State* L) {
    push_big(L, to_bigdecimal(L, 1).mul(to_bigdecimal(L, 2)));
    return 1;
}
int l_big_div(lua_State* L) {
    const BigDecimal b = to_bigdecimal(L, 2);
    if (b.is_zero()) return luaL_error(L, "big division by zero");
    const int def = static_cast<int>(lua_tointeger(L, lua_upvalueindex(1)));
    const int precision = static_cast<int>(luaL_optinteger(L, 3, def));
    push_big(L, to_bigdecimal(L, 1).div(b, precision));
    return 1;
}
int l_big_unm(lua_State* L) {
    push_big(L, to_bigdecimal(L, 1).neg());
    return 1;
}
int l_big_eq(lua_State* L) {
    lua_pushboolean(L, to_bigdecimal(L, 1).compare(to_bigdecimal(L, 2)) == 0);
    return 1;
}
int l_big_lt(lua_State* L) {
    lua_pushboolean(L, to_bigdecimal(L, 1).compare(to_bigdecimal(L, 2)) < 0);
    return 1;
}
int l_big_le(lua_State* L) {
    lua_pushboolean(L, to_bigdecimal(L, 1).compare(to_bigdecimal(L, 2)) <= 0);
    return 1;
}
int l_big_tostring(lua_State* L) {
    lua_pushstring(L, to_bigdecimal(L, 1).to_string().c_str());
    return 1;
}

// --- 实例方法（__index 表） ---
int l_big_method_add(lua_State* L) { push_big(L, to_bigdecimal(L, 1).add(to_bigdecimal(L, 2))); return 1; }
int l_big_method_sub(lua_State* L) { push_big(L, to_bigdecimal(L, 1).sub(to_bigdecimal(L, 2))); return 1; }
int l_big_method_mul(lua_State* L) { push_big(L, to_bigdecimal(L, 1).mul(to_bigdecimal(L, 2))); return 1; }
int l_big_method_div(lua_State* L) {
    const BigDecimal b = to_bigdecimal(L, 2);
    if (b.is_zero()) return luaL_error(L, "big division by zero");
    const int def = static_cast<int>(lua_tointeger(L, lua_upvalueindex(1)));
    const int precision = static_cast<int>(luaL_optinteger(L, 3, def));
    push_big(L, to_bigdecimal(L, 1).div(b, precision));
    return 1;
}
int l_big_method_pow(lua_State* L) {
    const int exp = static_cast<int>(luaL_checkinteger(L, 2));
    const int def = static_cast<int>(lua_tointeger(L, lua_upvalueindex(1)));
    const int precision = static_cast<int>(luaL_optinteger(L, 3, def));
    push_big(L, to_bigdecimal(L, 1).pow(exp, precision));
    return 1;
}
int l_big_method_sqrt(lua_State* L) {
    const int def = static_cast<int>(lua_tointeger(L, lua_upvalueindex(1)));
    const int precision = static_cast<int>(luaL_optinteger(L, 2, def));
    push_big(L, to_bigdecimal(L, 1).sqrt(precision));
    return 1;
}
int l_big_method_abs(lua_State* L) { push_big(L, to_bigdecimal(L, 1).abs()); return 1; }
int l_big_method_neg(lua_State* L) { push_big(L, to_bigdecimal(L, 1).neg()); return 1; }
int l_big_method_floor(lua_State* L) { push_big(L, to_bigdecimal(L, 1).floor()); return 1; }
int l_big_method_ceil(lua_State* L) { push_big(L, to_bigdecimal(L, 1).ceil()); return 1; }
int l_big_method_round(lua_State* L) { push_big(L, to_bigdecimal(L, 1).round()); return 1; }
int l_big_method_cmp(lua_State* L) {
    lua_pushinteger(L, to_bigdecimal(L, 1).compare(to_bigdecimal(L, 2)));
    return 1;
}
int l_big_method_to_float(lua_State* L) {
    lua_pushnumber(L, to_bigdecimal(L, 1).to_double());
    return 1;
}
int l_big_method_to_string(lua_State* L) {
    BigDecimal v = to_bigdecimal(L, 1);
    if (lua_gettop(L) >= 2) {
        const int places = static_cast<int>(luaL_checkinteger(L, 2));
        v = v.set_precision(places);
    }
    lua_pushstring(L, v.to_string().c_str());
    return 1;
}
int l_big_method_precision(lua_State* L) {
    BigDecimal v = to_bigdecimal(L, 1);
    if (lua_gettop(L) >= 2) {
        const int n = static_cast<int>(luaL_checkinteger(L, 2));
        push_big(L, v.set_precision(n));
        return 1;
    }
    lua_pushinteger(L, v.scale());
    return 1;
}
int l_big_method_is_zero(lua_State* L) {
    lua_pushboolean(L, to_bigdecimal(L, 1).is_zero());
    return 1;
}
int l_big_method_sign(lua_State* L) {
    lua_pushinteger(L, to_bigdecimal(L, 1).sign());
    return 1;
}

const luaL_Reg kBigMethods[] = {
    {"add", l_big_method_add},
    {"sub", l_big_method_sub},
    {"mul", l_big_method_mul},
    {"div", l_big_method_div},
    {"pow", l_big_method_pow},
    {"sqrt", l_big_method_sqrt},
    {"abs", l_big_method_abs},
    {"neg", l_big_method_neg},
    {"floor", l_big_method_floor},
    {"ceil", l_big_method_ceil},
    {"round", l_big_method_round},
    {"cmp", l_big_method_cmp},
    {"to_float", l_big_method_to_float},
    {"to_string", l_big_method_to_string},
    {"precision", l_big_method_precision},
    {"is_zero", l_big_method_is_zero},
    {"sign", l_big_method_sign},
    {nullptr, nullptr}
};

} // namespace

void register_gryce_math_library(lua_State* L) {
    // 扩展标准 math 表
    lua_getglobal(L, "math");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
    }
    luaL_setfuncs(L, kMathExt, 0);
    lua_setglobal(L, "math");
}

void register_big_number_library(lua_State* L) {
    // 默认精度（小数位）
    lua_pushinteger(L, 32);
    const int def_prec_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    // 创建 metatable
    luaL_newmetatable(L, k_big_metatable);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, l_big_add);
    lua_setfield(L, -2, "__add");
    lua_pushcfunction(L, l_big_sub);
    lua_setfield(L, -2, "__sub");
    lua_pushcfunction(L, l_big_mul);
    lua_setfield(L, -2, "__mul");
    lua_rawgeti(L, LUA_REGISTRYINDEX, def_prec_ref);
    lua_pushcclosure(L, l_big_div, 1);
    lua_setfield(L, -2, "__div");
    lua_pushcfunction(L, l_big_unm);
    lua_setfield(L, -2, "__unm");
    lua_pushcfunction(L, l_big_eq);
    lua_setfield(L, -2, "__eq");
    lua_pushcfunction(L, l_big_lt);
    lua_setfield(L, -2, "__lt");
    lua_pushcfunction(L, l_big_le);
    lua_setfield(L, -2, "__le");
    lua_pushcfunction(L, l_big_tostring);
    lua_setfield(L, -2, "__tostring");

    // 实例方法表（带默认精度 upvalue）
    lua_rawgeti(L, LUA_REGISTRYINDEX, def_prec_ref);
    lua_newtable(L);
    lua_pushvalue(L, -2);                 // def_prec 副本
    luaL_setfuncs(L, kBigMethods, 1);
    lua_setfield(L, -3, "__index");
    lua_pop(L, 2);                        // def_prec + metatable

    // big 模块表
    lua_newtable(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, def_prec_ref);
    lua_pushcclosure(L, l_big_new, 1);
    lua_setfield(L, -2, "decimal");
    lua_rawgeti(L, LUA_REGISTRYINDEX, def_prec_ref);
    lua_pushcclosure(L, l_big_new, 1);
    lua_setfield(L, -2, "new");
    lua_pushcfunction(L, l_big_int);
    lua_setfield(L, -2, "int");
    lua_pushcfunction(L, l_big_is_big);
    lua_setfield(L, -2, "is_big");
    lua_pushcfunction(L, l_big_abs);
    lua_setfield(L, -2, "abs");
    lua_pushcfunction(L, l_big_neg);
    lua_setfield(L, -2, "neg");
    lua_pushcfunction(L, l_big_floor);
    lua_setfield(L, -2, "floor");
    lua_pushcfunction(L, l_big_ceil);
    lua_setfield(L, -2, "ceil");
    lua_pushcfunction(L, l_big_round);
    lua_setfield(L, -2, "round");
    lua_rawgeti(L, LUA_REGISTRYINDEX, def_prec_ref);
    lua_pushcclosure(L, l_big_sqrt, 1);
    lua_setfield(L, -2, "sqrt");
    lua_rawgeti(L, LUA_REGISTRYINDEX, def_prec_ref);
    lua_pushcclosure(L, l_big_pow, 1);
    lua_setfield(L, -2, "pow");
    lua_pushcfunction(L, l_big_cmp);
    lua_setfield(L, -2, "cmp");
    lua_pushcfunction(L, l_big_to_float);
    lua_setfield(L, -2, "to_float");
    lua_pushcfunction(L, l_big_to_string);
    lua_setfield(L, -2, "to_string");
    lua_rawgeti(L, LUA_REGISTRYINDEX, def_prec_ref);
    lua_pushcclosure(L, l_big_pi, 1);
    lua_setfield(L, -2, "pi");
    lua_rawgeti(L, LUA_REGISTRYINDEX, def_prec_ref);
    lua_pushcclosure(L, l_big_e, 1);
    lua_setfield(L, -2, "e");
    lua_rawgeti(L, LUA_REGISTRYINDEX, def_prec_ref);
    lua_pushcclosure(L, l_big_default_precision, 1);
    lua_setfield(L, -2, "precision");
    lua_setglobal(L, "big");

    luaL_unref(L, LUA_REGISTRYINDEX, def_prec_ref);
}

} // namespace gryce_engine::script
