#pragma once

// ---------------------------------------------------------------------------
// GryceGC 命令行参数解析（手写，无第三方依赖）
//
// 设计目标：
//   - 零依赖、不引入 CLI 库（项目偏好）
//   - 风格与原有 Python 工具链相近（GNU 长选项：--name / --name=value）
//   - 支持重复选项（--extra-dir a --extra-dir b），-- 透传，错误信息中文
//
// 形式语法（单个命令的 argv 子集）：
//   --name           视为 flag（无值）
//   --name=value     视为带值选项
//   --name value     视为带值选项（value 不以 '-' 开头才合并）
//   --key -5         视为 flag（避免吞负数；想传 -5 用 --key=-5）
//   --               透传起点；后续 token 一律进 passthrough
//   其它 -X         报错：未知短选项
//   其它 token       视为位置参数
// ---------------------------------------------------------------------------

#include <map>
#include <set>
#include <string>
#include <vector>

namespace grycegc {

struct Options {
  // 原始上下文
  int argc = 0;
  const char* const* argv = nullptr;

  // 解析结果（仅限 command 自身的 argv 子集，不含命令名本身）
  std::map<std::string, std::string> single;                  // --name value / --name=value
  std::map<std::string, std::vector<std::string>> all;        // 全部值（多 --name 累加）
  std::set<std::string> flags;                                // --flag（无值）
  std::vector<std::string> positionals;
  std::vector<std::string> passthrough;                       // -- 之后

  // 工具
  bool has(const std::string& key) const;                     // 出现过 --key（任何形式）
  bool is_flag(const std::string& key) const;                 // 出现为 flag 形式
  std::string get(const std::string& key, const std::string& def = {}) const;
  std::vector<std::string> get_all(const std::string& key) const;
  int get_int(const std::string& key, int def = 0) const;
};

// 解析 argv[1..argc-1]，提取 command 与对应 Options。
// 失败返回 false（error 含中文错误信息）。command==空表示无命令（main 走 help）。
// argv[0] 通常是 exe 路径，被解析时跳过。
bool parse_argv(int argc, const char* const argv[],
                std::string& command, Options& opts, std::string& error);

} // namespace grycegc
