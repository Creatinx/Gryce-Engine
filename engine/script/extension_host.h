// ===========================================================================
// engine/script/extension_host.h
// 原生扩展装载：把 GryceExtensionApi 虚表交给扩展动态库，并托管"原生函数 ->
// QuickJS CFunction"的桥接。
//
// 生命周期：扩展在 context 创建后、main.js 求值前加载一次；函数表项在进程
// 内只增不减（条目很小），重载 main.js 不会重复注册。
// ===========================================================================

#pragma once

#include <string>

extern "C" {
#include "quickjs.h"
}

#include "api/extension_api.h"
#include "export.h"

namespace gryce_engine::script {

// 构造扩展函数表（js_ctx 为该表的上下文）。
GRYCE_API GryceExtensionApi make_extension_api(JSContext* ctx);

// 加载 path 指向的动态库并调用其 gryce_register_extension。失败返回 false，
// error 带原因（文件不存在 / 缺入口 / 版本不符）。Web 构建恒失败。
GRYCE_API bool load_extension_library(JSContext* ctx, const std::string& path,
                                      std::string* error);

} // namespace gryce_engine::script
