#pragma once

// MathBridge — 将 math.* 扩展注册到 ScriptVM 的 JS 全局作用域
//
// 移植自 Lua 的 lua_math_bindings.cpp，提供游戏常用数学函数

#include <quickjs/quickjs.h>

#include "export.h"

namespace GryceEngineUtils::script {

GRYCE_API void register_math_bindings(JSContext* ctx);

} // namespace GryceEngineUtils::script