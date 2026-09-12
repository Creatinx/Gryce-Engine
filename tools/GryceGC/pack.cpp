#include "pack.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

#include "crypto.h"
#include "resources/pak_bundle.h"

namespace fs = std::filesystem;

namespace gryce_engine::gc {

bool write_bundle(const std::vector<FileEntry>& files, const fs::path& output_path,
                  size_t& entry_count) {
    // 使用 PakWriter（GPAK v4）：随机 Base64 存储名 + manifest 映射逻辑路径；
    // 设置了 32 字节全局密钥时对 data 区做 ChaCha20 加密。
    gryce_engine::resources::PakWriter writer;
    bool ok = true;
    for (const FileEntry& file : files) {
        if (!writer.add_file(file.internal_path, file.source_path.string())) {
            std::cerr << "[grycegc] ERROR: PakWriter::add_file('" << file.internal_path << "') failed\n";
            ok = false;
            break;
        }
    }
    if (ok && !writer.write(output_path.string())) {
        std::cerr << "[grycegc] ERROR: PakWriter::write('" << output_path << "') failed\n";
        ok = false;
    }
    if (!ok) return false;

    // 读回验证：打开 + manifest 数量一致 + 每个文件解密环回比对（加密写/解密读必须一致）。
    gryce_engine::resources::PakReader reader;
    if (!reader.open(output_path.string())) {
        std::cerr << "[grycegc] ERROR: .gpkg verification failed to open " << output_path << "\n";
        return false;
    }
    if (reader.manifest().size() != files.size()) {
        std::cerr << "[grycegc] ERROR: .gpkg verification mismatch in " << output_path << "\n";
        return false;
    }
    for (const FileEntry& file : files) {
        const std::vector<uint8_t> restored = reader.read(file.internal_path);
        std::ifstream ifs(file.source_path, std::ios::binary);
        const std::vector<uint8_t> orig((std::istreambuf_iterator<char>(ifs)),
                                        std::istreambuf_iterator<char>());
        bool content_ok = false;
        if (restored.size() == orig.size()) {
            content_ok = std::equal(restored.begin(), restored.end(), orig.begin());
        }
        if (!content_ok) {
            std::cerr << "[grycegc] ERROR: decrypt mismatch for " << file.internal_path
                      << " in " << output_path << "\n";
            return false;
        }
    }
    entry_count += files.size();
    const uintmax_t size = fs::file_size(output_path);
    std::printf("[grycegc] %s: %zu files, %.2f MiB (random Base64 names, manifest)\n",
                output_path.filename().string().c_str(), reader.manifest().size(),
                static_cast<double>(size) / (1024.0 * 1024.0));
    return true;
}

bool write_project_data(const fs::path& project, const fs::path& out_dir,
                        const std::string& name, const std::string& author,
                        const std::vector<FileEntry>& files,
                        const std::string& enc_key_hex) {
    std::error_code ec;

    // 1) 源项目配置：项目清单 + 运行时设置。优先 project.data；兼容旧
    //    project.gproj（清单）与 project_settings.json（运行时设置），后者并入顶层
    //    （键冲突时按读取顺序后者覆盖前者）。
    nlohmann::json merged = nlohmann::json::object();
    for (const char* cfg : {"project.data", "project.gproj", "project_settings.json"}) {
        const fs::path p = project / cfg;
        if (!fs::is_regular_file(p, ec)) continue;
        try {
            std::ifstream in(p);
            nlohmann::json j = nlohmann::json::parse(in, nullptr, false); // no throw
            if (j.is_discarded()) {
                std::cerr << "[grycegc] warning: failed to parse " << p << ", skipping\n";
                continue;
            }
            if (j.is_object()) {
                for (auto it = j.begin(); it != j.end(); ++it) merged[it.key()] = it.value();
            }
        } catch (...) {
            std::cerr << "[grycegc] warning: failed to parse " << p << ", skipping\n";
        }
    }
    if (merged.empty()) {
        merged["name"] = name;
    }

    // 2) Source records + 64-byte SHA-512 key（对源文件记录做摘要）。
    std::string records;
    nlohmann::json sources = nlohmann::json::array();
    for (const FileEntry& file : files) {
        const std::string bytes = read_file_bytes(file.source_path);
        const std::string digest = sha256_hex(bytes.data(), bytes.size());
        if (digest.empty()) {
            std::cerr << "[grycegc] ERROR: failed to hash " << file.internal_path << "\n";
            return false;
        }
        records += file.internal_path + ":" +
                   std::to_string(fs::file_size(file.source_path)) + ":" +
                   digest + "\n";
        nlohmann::json rec;
        rec["path"] = file.internal_path;
        rec["sha256"] = digest;
        rec["size"] = fs::file_size(file.source_path);
        sources.push_back(rec);
    }
    const std::string key = sha512_hex(records.data(), records.size());
    if (key.empty()) {
        std::cerr << "[grycegc] ERROR: failed to derive project.data key\n";
        return false;
    }

    std::time_t now = std::time(nullptr);
    char created[64] = {};
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    std::strftime(created, sizeof(created), "%Y-%m-%dT%H:%M:%S", &local);

    // 3) 合并打包元数据（原 gdata 字段）写入同一份 project.data。
    merged["format"] = "gryce_project_data";
    merged["version"] = 1;
    merged["project"] = name;
    merged["author"] = author;
    merged["created"] = created;
    merged["tool"] = "grycegc";
    merged["key_sha512_hex"] = key;             // 64 bytes, hex-encoded
    merged["enc_key_hex"] = enc_key_hex;        // ChaCha20 32B 密钥（hex）
    merged["sources"] = sources;

    const fs::path out = out_dir / "project.data";
    std::ofstream out_fs(out);
    if (!out_fs) {
        std::cerr << "[grycegc] ERROR: failed to write " << out << "\n";
        return false;
    }
    out_fs << merged.dump(2) << "\n";
    if (out_fs.good()) {
        std::printf("[grycegc] project.data: %zu source records + 64-byte SHA-512 key + author '%s'"
                    " (manifest, settings & metadata merged at output root)\n",
                    files.size(), author.c_str());
    }
    return out_fs.good();
}

} // namespace gryce_engine::gc