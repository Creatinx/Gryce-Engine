// ---------------------------------------------------------------------------
// GryceGC 主入口：子命令分派
// ---------------------------------------------------------------------------

#include "cli.h"
#include "commands/commands.h"
#include "commands/registry.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

int main(int argc, char** argv) {
    // Windows console UTF-8 输出（避免中文乱码）
#if defined(_WIN32)
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
#endif

    std::string command, error;
    grycegc::Options opts;
    if (!grycegc::parse_argv(argc, argv, command, opts, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 2;
    }

    // 处理全局 --help / -h：转发到 cmd_help，topic 由第一个非选项 token 决定
    if (opts.has("help") || opts.has("h")) {
        grycegc::Options help_opts;
        help_opts.argc = 0;
        if (!command.empty()) help_opts.positionals.push_back(command);
        return grycegc::cmd_help(help_opts);
    }

    // 无命令：走 help
    if (command.empty()) {
        return grycegc::cmd_help(grycegc::Options{});
    }

    const auto* cmd = grycegc::find_command(command);
    if (!cmd) {
        std::fprintf(stderr, "[错误] 未知的子命令：'%s'\n", command.c_str());
        grycegc::cmd_help(grycegc::Options{});
        return 2;
    }
    return cmd->fn(opts);
}
