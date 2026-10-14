#include "../cli.h"
#include "registry.h"
#include "commands.h"

#include <cstdio>
#include <string>

namespace grycegc {

namespace {

void print_usage() {
    std::printf("GryceGC — Gryce-Engine 工具链 CLI（C++ 重写）\n");
    std::printf("用法: GryceGC <command> [options]\n");
    std::printf("运行 'GryceGC help <command>' 查看命令详情。\n\n");
    std::printf("可用命令:\n");
    const auto& t = all_commands();
    // 计算名字列宽
    size_t w = 0;
    for (const auto& c : t) w = std::max(w, c.name.size());
    for (const auto& c : t) {
        std::printf("  %-*s   %s\n", static_cast<int>(w), c.name.c_str(), c.summary.c_str());
    }
}

} // namespace

int cmd_version(const Options&) {
#ifdef GRYCE_GC_VERSION
    std::printf("GryceGC %s\n", GRYCE_GC_VERSION);
#else
    std::printf("GryceGC (unknown)\n");
#endif
    return 0;
}

int cmd_help(const Options& opts) {
    if (opts.positionals.empty()) {
        print_usage();
        return 0;
    }
    const auto* cmd = find_command(opts.positionals[0]);
    if (!cmd) {
        std::fprintf(stderr, "[错误] 未知的子命令：'%s'\n\n", opts.positionals[0].c_str());
        print_usage();
        return 2;
    }
    std::printf("GryceGC %s — %s\n", cmd->name.c_str(), cmd->summary.c_str());
    std::printf("%s", cmd->help.c_str());
    return 0;
}

} // namespace grycegc
