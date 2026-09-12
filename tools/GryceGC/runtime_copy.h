// 发布目录运行时组装：拷贝引擎 core DLL、GLFW/GLEW/MinGW/MSVC CRT 运行时与七人 exe。
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace gryce_engine::gc {

// 若 src_dir/name 存在则拷贝到 dst_dir，并把文件名追加到 copied；不存在返回 false。
bool copy_file_if_exists(const std::filesystem::path& src_dir, const std::string& name,
                         const std::filesystem::path& dst_dir,
                         std::vector<std::string>& copied);

// 组装 <out_dir>（含 runtime/ 子目录）的运行时与模板 exe。
// 依赖由 bin_dir 前缀扫描得到；MinGW 构建的 DLL 会镜像到输出根以满足进程启动期依赖。
bool copy_runtime(const std::filesystem::path& build_dir,
                  const std::filesystem::path& bin_dir, bool debug,
                  const std::filesystem::path& out_dir,
                  const std::filesystem::path& exe, const std::string& name,
                  std::vector<std::string>& copied);

} // namespace gryce_engine::gc