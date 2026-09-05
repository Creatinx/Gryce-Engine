#pragma once

// BigBridge — 将 big.* 模块注册到 ScriptVM 的 JS 全局作用域
//
// 移植自 Lua 的 big_number 模块，提供 BigInt / BigDecimal 的 JS 绑定
// 用法：
//   big.int("12345678901234567890")
//   big.decimal("3.14159265358979323846")
//   big.int("100").add(big.int("200"))
//   big.decimal("1.5").add(big.decimal("2.5"))

#include <quickjs/quickjs.h>

#include "export.h"

namespace GryceEngineUtils::script {

GRYCE_API void register_big_bindings(JSContext* ctx);

} // namespace GryceEngineUtils::script