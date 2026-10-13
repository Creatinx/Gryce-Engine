// M2–M5 milestone 未实现命令的占位实现。
// 每个返回 2 并打印中文提示；实现在对应 milestone 阶段替换。

#include "../cli.h"

#include <cstdio>

namespace grycegc {

namespace {
int not_implemented(const Options& opts, const char* name) {
    (void)opts;
    std::fprintf(stderr, "[未实现] GryceGC %s — 见 toolchain/grycegc/ 的里程碑计划。\n", name);
    return 2;
}
} // namespace

int cmd_package(const Options& o)    { return not_implemented(o, "package"); }
int cmd_run(const Options& o)        { return not_implemented(o, "run"); }
int cmd_copyright(const Options& o)  { return not_implemented(o, "copyright"); }
int cmd_meta(const Options& o)       { return not_implemented(o, "meta"); }
int cmd_encrypt(const Options& o)    { return not_implemented(o, "encrypt"); }
int cmd_decrypt(const Options& o)    { return not_implemented(o, "decrypt"); }
int cmd_keygen(const Options& o)     { return not_implemented(o, "keygen"); }
int cmd_pak(const Options& o)        { return not_implemented(o, "pak"); }
int cmd_debug(const Options& o)      { return not_implemented(o, "debug"); }
int cmd_bench(const Options& o)      { return not_implemented(o, "bench"); }
int cmd_font(const Options& o)       { return not_implemented(o, "font"); }
int cmd_build(const Options& o)      { return not_implemented(o, "build"); }

} // namespace grycegc
