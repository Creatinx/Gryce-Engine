#pragma once

// ---------------------------------------------------------------------------
// GryceGC meta：元数据读写
//
// 三个目标层级：
//   1) 目录：gryce.meta.json（UTF-8 JSON，合并写，不破坏既有键）
//   2) 资源项目目录：project.data（key=value INI 风格，仅合并不删除键）
//   3) PE 文件（.exe/.dll）：EOF trailer（GRYCETL1 + JSON payload，不影响加载）
//
// 刻意不做 PE 节表手术 / VERSIONINFO 资源改写（那是 M5 的 copyright 章职责）。
// ---------------------------------------------------------------------------

#include <optional>
#include <string>
#include <vector>

#include <filesystem>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace grycegc {

// gryce.meta.json 的标准字段（顶层）
struct MetaDoc {
    std::string name;         // 包/工程名
    std::string version;      // 语义版本
    std::string copyright;    // 版权行文本
    std::string license;      // SPDX 或自定义 license 标识
    std::string build_id;     // git commit 戳（M5 meta --stamp-git 写入）
    std::string built_at;     // ISO 8601
    std::string toolchain;    // 生成工具标识（如 "GryceGC 1.0.0"）
    std::vector<std::string> tags;
    nlohmann::json extra;     // 任意附加键（不丢信息）

    static MetaDoc from_json(const nlohmann::json& j);
    nlohmann::json to_json() const;
};

// 目录级：读/写/合并 gryce.meta.json
MetaDoc meta_read_dir(const fs::path& dir);
bool meta_write_dir(const fs::path& dir, const MetaDoc& doc);

// project.data 合并：把 k=v 合并进文件（存在则更新，不存在则追加；保留注释行）
bool meta_merge_project_data(const fs::path& project_dir,
                             const std::vector<std::pair<std::string, std::string>>& kv);

// 从 project.data 读单个键
std::optional<std::string> project_data_get(const fs::path& project_dir,
                                            const std::string& key);

// PE trailer JSON 读写（payload 为 JSON 文本；由调用方保证）
bool meta_write_pe_trailer(const fs::path& pe, const nlohmann::json& payload);
std::optional<nlohmann::json> meta_read_pe_trailer(const fs::path& pe);

} // namespace grycegc
