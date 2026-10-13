#pragma once

// 全部子命令的入口声明。每个 milestone 都在这里加一行。

namespace grycegc {

class Options;

int cmd_version(const Options&);
int cmd_help(const Options&);
int cmd_deps(const Options&);
int cmd_verify(const Options&);

// 以下将在后续 milestone 实现（M2–M5），提前声明以便注册表统一调度
int cmd_package(const Options&);
int cmd_run(const Options&);
int cmd_copyright(const Options&);
int cmd_meta(const Options&);
int cmd_encrypt(const Options&);
int cmd_decrypt(const Options&);
int cmd_keygen(const Options&);
int cmd_pak(const Options&);
int cmd_debug(const Options&);
int cmd_bench(const Options&);
int cmd_font(const Options&);
int cmd_build(const Options&);

} // namespace grycegc
