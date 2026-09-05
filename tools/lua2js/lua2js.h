#pragma once

// lua2js — Lua 到 ES Module JavaScript 的基础语法迁移工具
//
// 定位：历史/存量 Lua 脚本的导入辅助，非主路径。Lua 运行时已完全移除，
// 引擎脚本统一为 QuickJS。本工具覆盖基础语法：
//   - 注释：-- / --[[ ]] -> // / /* */
//   - 顶层函数：function name() end -> export function name() {}
//   - local function：-> function() {}
//   - local 变量：local x = {a=1} -> const x = {a:1}
//   - 表构造：{a=1, [k]=v} -> {a:1, [k]:v}
//   - 运算符：and/or/not/~=/==/.. /nil
//   - 控制流：if/elseif/else/end、数值 for、while、do 块
//   - 长度：#t -> __len(t)（注入 __len helper）
//
// 复杂闭包、元表、协程、多返回值等需手动重写（转换输出带 TODO 标记）。

#include <string>
#include <vector>

namespace lua2js {

struct ConvertResult {
    bool ok = true;
    std::string output;
    std::vector<std::string> warnings; // 需要人工确认的位置
};

/// 将 Lua 源码转换为 ES Module 风格的 JS 源码。
/// 失败时 ok=false 并附带错误信息（在 warnings 中）。
ConvertResult convert(const std::string& luaSource);

} // namespace lua2js
