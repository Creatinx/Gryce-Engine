// GryceLua2JS — Lua 到 JS 迁移辅助 CLI
//
// 用法：
//   lua2js <input.lua> [output.js]
// 无 output 时输出到 stdout。

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "lua2js.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
            "用法: lua2js <input.lua> [output.js]\n"
            "将 Lua 基础语法转换为 ES Module 风格 JS（存量脚本导入辅助）。\n"
            "复杂闭包/元表/协程/多返回值需按警告手动重写。\n");
        return 1;
    }

    std::ifstream in(argv[1], std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "无法打开输入文件: %s\n", argv[1]);
        return 1;
    }
    std::ostringstream ss;
    ss << in.rdbuf();

    const lua2js::ConvertResult result = lua2js::convert(ss.str());
    if (!result.ok) {
        std::fprintf(stderr, "转换失败\n");
        return 1;
    }

    for (const auto& w : result.warnings) {
        std::fprintf(stderr, "[lua2js] 注意: %s\n", w.c_str());
    }

    if (argc >= 3) {
        std::ofstream out(argv[2], std::ios::binary);
        if (!out) {
            std::fprintf(stderr, "无法写入输出文件: %s\n", argv[2]);
            return 1;
        }
        out << result.output;
        std::fprintf(stderr, "已写入 %s\n", argv[2]);
    } else {
        std::cout << result.output;
    }
    return 0;
}
