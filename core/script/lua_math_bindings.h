#pragma once

struct lua_State;

namespace gryce_engine::script {

// 注册 GryceSRT 数学扩展（math.*：lerp/clamp/缓动等）与
// 超高精度数值模块（big.*：BigInt / BigDecimal）。
void register_gryce_math_library(lua_State* L);
void register_big_number_library(lua_State* L);

} // namespace gryce_engine::script
