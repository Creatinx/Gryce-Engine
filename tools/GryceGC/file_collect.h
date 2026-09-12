// 资源文件收集：遍历项目与 core 默认 shader，产出可打包的 FileEntry 列表。
#pragma once

#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

namespace gryce_engine::gc {

// 一个待打包文件：internal_path 为项目根相对路径（正斜杠），source_path 为磁盘源文件。
struct FileEntry {
    std::string internal_path;
    std::filesystem::path source_path;
};

// 读取整个文件为字节串。
std::string read_file_bytes(const std::filesystem::path& path);

// 收集项目根下所有可打包资源（剔除构建/源码/元数据目录与文件）。
std::vector<FileEntry> collect_project_files(const std::filesystem::path& project_root);

// 收集 core 默认 shader 源文件（internal 前缀 "shaders/"），跳过项目已覆盖的同 internal
// 路径文件，保证任意 shader 键只有一份（项目覆盖优先、core 兜底）。
std::vector<FileEntry> collect_core_shader_files(
    const std::filesystem::path& engine_shaders,
    const std::unordered_set<std::string>& project_paths);

} // namespace gryce_engine::gc