#pragma once

// ---------------------------------------------------------------------------
// GryceGC process：跨平台子进程启动
//
// 用于 run / debug / bench 三个命令：
//   - Windows：CreateProcessW，可注入/覆盖环境变量，支持输出捕获；
//   - POSIX：fork + execve（environ 复制后修改）。
// 设计要点：
//   - 环境注入走"拷贝父环境 + set/override"，PATH 追加目录时按平台分隔符；
//   - run_process 直接继承父控制台输出；run_process_capture 捕获 stdout；
//   - 退出码原样传播（子进程被信号杀死的场景按 0x80+signal 近似处理）。
// ---------------------------------------------------------------------------

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <filesystem>

namespace fs = std::filesystem;

namespace grycegc {

// 启动结果
struct ProcessResult {
    bool spawned = false;     // 进程是否成功创建
    int exit_code = -1;       // 退出码（spawned=false 时为 -1）
    std::string spawn_error;  // 创建失败原因（spawned=false 时非空）
    std::string stdout_text;  // 捕获模式下的 stdout（run_process_capture）
};

// 环境变量覆盖列表：name -> value（value 为空串表示删除该变量）
using EnvOverrides = std::vector<std::pair<std::string, std::string>>;

// 把目录追加到 PATH 前面（run.bat 语义：dll/ 优先于系统 PATH）
std::string prepend_to_path_env(const std::string& dir, const std::string& old_path);

// 启动子进程，继承父进程 stdin/stdout/stderr
ProcessResult run_process(const fs::path& exe, const std::vector<std::string>& args,
                          const EnvOverrides& env = {});

// 启动子进程并捕获 stdout（stderr 仍继承父进程）
ProcessResult run_process_capture(const fs::path& exe, const std::vector<std::string>& args,
                                  const EnvOverrides& env = {});

} // namespace grycegc
