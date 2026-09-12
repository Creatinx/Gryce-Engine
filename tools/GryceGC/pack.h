// 打包：把文件写入单个 .gpkg 并做读回验证；生成游戏根唯一的 project.data 配置。
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "file_collect.h"

namespace gryce_engine::gc {

// 将 files 打包写入 output_path（GPAK v4），并读回验证 manifest 数量与每个文件解密环回。
// entry_count 累加实际写入的条目数。
bool write_bundle(const std::vector<FileEntry>& files,
                  const std::filesystem::path& output_path, size_t& entry_count);

// 生成游戏根的 project.data（合并源项目清单/运行时设置 + 打包元数据 + enc_key_hex）。
bool write_project_data(const std::filesystem::path& project,
                        const std::filesystem::path& out_dir,
                        const std::string& name, const std::string& author,
                        const std::vector<FileEntry>& files,
                        const std::string& enc_key_hex);

} // namespace gryce_engine::gc