// ---------------------------------------------------------------------------
// GryceGC pak：资源包 create / list / extract / verify（M3）
//
// 复用引擎实现（GryceGC 直接编译 engine/resources 源码）：
//   .gpack → GPackWriter / GPackReader（GPAK v1，包内路径即原始路径）
//   .pak   → PakWriter / PakReader（GPAK v3 随机名 + manifest；--key 时 v4 加密）
//
// 用法:
//   GryceGC pak create --out p.gpack [输入文件或目录...] [--key hex]
//   GryceGC pak list --file p.gpack|p.pak [--key hex]
//   GryceGC pak extract --file p.gpack|p.pak --out dir [--key hex]
//   GryceGC pak verify --file p.gpack|p.pak [--key hex]
// ---------------------------------------------------------------------------

#include "../cli.h"
#include "../commands/commands.h"
#include "../core/fsutil.h"
#include "resources/gpack_bundle.h"
#include "resources/pak_bundle.h"
#include "utils/crypto/crc32.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace grycegc {

namespace {

bool parse_key_hex(const std::string& spec, std::string& key) {
    // 接受 @file / 直接 hex（pak 密钥按 32 字节原样传给引擎）
    if (spec.rfind("@", 0) == 0) {
        auto bytes = read_all(spec.substr(1));
        if (bytes.size() != 32) return false;
        key.assign(reinterpret_cast<const char*>(bytes.data()), 32);
        return true;
    }
    if (spec.size() != 64) return false;
    std::vector<uint8_t> raw;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < spec.size(); i += 2) {
        const int hi = nib(spec[i]), lo = nib(spec[i + 1]);
        if (hi < 0 || lo < 0) return false;
        raw.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    if (raw.size() != 32) return false;
    key.assign(reinterpret_cast<const char*>(raw.data()), 32);
    return true;
}

std::string normalize_internal(const fs::path& p) {
    std::string s = p.generic_string();
    return s;
}

// 收集输入：文件以父目录为基准；目录以自身为基准（包内路径相对该目录）
void collect_inputs(const fs::path& input, const fs::path& base,
                    std::vector<std::pair<std::string, fs::path>>& out) {
    std::error_code ec;
    if (fs::is_regular_file(input, ec)) {
        out.emplace_back(normalize_internal(input.lexically_relative(base)), input);
        return;
    }
    if (fs::is_directory(input, ec)) {
        for (auto it = fs::recursive_directory_iterator(
                 input, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            if (!it->is_regular_file(ec)) continue;
            out.emplace_back(normalize_internal(it->path().lexically_relative(input)),
                             it->path());
        }
    }
}

int pak_create(const Options& o) {
    if (!o.has("out")) {
        std::fprintf(stderr, "[错误] pak create 需要 --out <file>\n");
        return 2;
    }
    const fs::path out = o.get("out");
    const std::string ext = out.extension().string();
    const bool is_pak = (ext == ".pak");

    std::vector<std::pair<std::string, fs::path>> files;
    for (const auto& pos : o.positionals) {
        if (pos == "create") continue;
        const fs::path input(pos);
        if (!fs::exists(input)) {
            std::fprintf(stderr, "[错误] 输入不存在：%s\n", pos.c_str());
            return 2;
        }
        const fs::path base = fs::is_directory(input) ? input : input.parent_path();
        collect_inputs(input, base, files);
    }
    if (files.empty()) {
        std::fprintf(stderr, "[错误] 没有输入文件（pak create --out x.gpack <文件或目录...>）\n");
        return 2;
    }
    std::sort(files.begin(), files.end());

    std::string key;
    if (o.has("key")) {
        if (!is_pak) {
            std::fprintf(stderr, "[错误] --key 加密仅支持 .pak（GPAK v4）\n");
            return 2;
        }
        if (!parse_key_hex(o.get("key"), key)) {
            std::fprintf(stderr, "[错误] --key 必须是 64 位十六进制或 @32字节文件\n");
            return 2;
        }
        gryce_engine::resources::set_pak_crypto_key(key);
    }

    bool ok = true;
    size_t total = 0;
    if (is_pak) {
        gryce_engine::resources::PakWriter w;
        for (const auto& [internal, src] : files) {
            if (!w.add_file(internal, src.string())) {
                std::fprintf(stderr, "[错误] 加入失败：%s\n", src.string().c_str());
                ok = false;
                break;
            }
            total += fs::file_size(src);
        }
        if (ok && !w.write(out.string())) ok = false;
    } else {
        gryce_engine::resources::GPackWriter w;
        for (const auto& [internal, src] : files) {
            if (!w.add_file(internal, src.string())) {
                std::fprintf(stderr, "[错误] 加入失败：%s\n", src.string().c_str());
                ok = false;
                break;
            }
            total += fs::file_size(src);
        }
        if (ok && !w.write(out.string())) ok = false;
    }
    if (o.has("key")) gryce_engine::resources::set_pak_crypto_key("");  // 复位全局密钥

    if (!ok) {
        std::fprintf(stderr, "[错误] 写出失败：%s\n", out.string().c_str());
        return 1;
    }
    std::printf("[完成] %s：%zu 个条目，源数据 %zu 字节%s\n", out.string().c_str(),
                files.size(), total, key.empty() ? "" : "（v4 ChaCha20 加密）");
    return 0;
}

int pak_list(const Options& o) {
    if (!o.has("file")) {
        std::fprintf(stderr, "[错误] pak list 需要 --file <p>\n");
        return 2;
    }
    const fs::path file = o.get("file");
    std::string key;
    if (o.has("key") && !parse_key_hex(o.get("key"), key)) {
        std::fprintf(stderr, "[错误] --key 格式不正确\n");
        return 2;
    }
    if (!key.empty()) gryce_engine::resources::set_pak_crypto_key(key);

    std::printf("包: %s\n", file.string().c_str());
    int rc = 0;
    if (file.extension() == ".pak") {
        gryce_engine::resources::PakReader r;
        if (!r.open(file.string())) {
            std::fprintf(stderr, "[错误] 打开失败（或密钥错误）\n");
            rc = 1;
        } else {
            for (const auto& m : r.manifest())
                std::printf("  %-48s -> %s\n", m.original_path.c_str(),
                            m.random_path.c_str());
            std::printf("共 %zu 个条目（manifest）\n", r.manifest().size());
        }
    } else {
        gryce_engine::resources::GPackReader r;
        if (!r.open(file.string())) {
            std::fprintf(stderr, "[错误] 打开失败\n");
            rc = 1;
        } else {
            for (const auto& e : r.entries())
                std::printf("  %-48s %10llu bytes  crc32=%08x\n", e.path.c_str(),
                            (unsigned long long)e.data_size, e.crc32);
            std::printf("共 %zu 个条目\n", r.entries().size());
        }
    }
    if (!key.empty()) gryce_engine::resources::set_pak_crypto_key("");
    return rc;
}

int pak_extract(const Options& o) {
    if (!o.has("file") || !o.has("out")) {
        std::fprintf(stderr, "[错误] pak extract 需要 --file <p> --out <dir>\n");
        return 2;
    }
    const fs::path file = o.get("file");
    const fs::path out_dir = o.get("out");
    std::string key;
    if (o.has("key") && !parse_key_hex(o.get("key"), key)) {
        std::fprintf(stderr, "[错误] --key 格式不正确\n");
        return 2;
    }
    if (!key.empty()) gryce_engine::resources::set_pak_crypto_key(key);

    size_t count = 0;
    if (file.extension() == ".pak") {
        gryce_engine::resources::PakReader r;
        if (!r.open(file.string())) {
            std::fprintf(stderr, "[错误] 打开失败（或密钥错误）\n");
            if (!key.empty()) gryce_engine::resources::set_pak_crypto_key("");
            return 1;
        }
        for (const auto& m : r.manifest()) {
            auto data = r.read(m.original_path);
            if (data.empty()) {
                std::fprintf(stderr, "[警告] 读不到数据：%s\n", m.original_path.c_str());
                continue;
            }
            const fs::path dst = out_dir / fs::path(m.original_path);
            mkdir_p(dst.parent_path());
            if (!write_binary(dst, data.data(), data.size())) {
                std::fprintf(stderr, "[错误] 写失败：%s\n", dst.string().c_str());
                if (!key.empty()) gryce_engine::resources::set_pak_crypto_key("");
                return 1;
            }
            ++count;
        }
    } else {
        gryce_engine::resources::GPackReader r;
        if (!r.open(file.string())) {
            std::fprintf(stderr, "[错误] 打开失败\n");
            if (!key.empty()) gryce_engine::resources::set_pak_crypto_key("");
            return 1;
        }
        for (const auto& e : r.entries()) {
            auto data = r.read(e.path);
            if (data.empty()) {
                std::fprintf(stderr, "[警告] 读不到数据：%s\n", e.path.c_str());
                continue;
            }
            const fs::path dst = out_dir / fs::path(e.path);
            mkdir_p(dst.parent_path());
            if (!write_binary(dst, data.data(), data.size())) {
                std::fprintf(stderr, "[错误] 写失败：%s\n", dst.string().c_str());
                if (!key.empty()) gryce_engine::resources::set_pak_crypto_key("");
                return 1;
            }
            ++count;
        }
    }
    if (!key.empty()) gryce_engine::resources::set_pak_crypto_key("");
    std::printf("[完成] 解出 %zu 个文件到 %s\n", count, out_dir.string().c_str());
    return 0;
}

int pak_verify(const Options& o) {
    if (!o.has("file")) {
        std::fprintf(stderr, "[错误] pak verify 需要 --file <p>\n");
        return 2;
    }
    const fs::path file = o.get("file");
    std::string key;
    if (o.has("key") && !parse_key_hex(o.get("key"), key)) {
        std::fprintf(stderr, "[错误] --key 格式不正确\n");
        return 2;
    }
    if (!key.empty()) gryce_engine::resources::set_pak_crypto_key(key);

    size_t bad = 0, total = 0;
    if (file.extension() == ".pak") {
        gryce_engine::resources::PakReader r;
        if (!r.open(file.string())) {
            std::fprintf(stderr, "[错误] 打开失败（或密钥错误）\n");
            if (!key.empty()) gryce_engine::resources::set_pak_crypto_key("");
            return 1;
        }
        for (const auto& e : r.entries()) {
            // v3/v4: entries 里是随机名；用 manifest 反查原始路径后读数据校验
            const std::string* orig = nullptr;
            for (const auto& m : r.manifest())
                if (m.random_path == e.path) { orig = &m.original_path; break; }
            if (!orig) continue;
            ++total;
            auto data = r.read(*orig);
            const uint32_t crc = gryce_engine::utils::crc32(data.data(), data.size());
            if (crc != e.crc32) {
                ++bad;
                std::printf("  [FAIL] %s（crc32 %08x != %08x）\n", orig->c_str(), crc, e.crc32);
            }
        }
    } else {
        gryce_engine::resources::GPackReader r;
        if (!r.open(file.string())) {
            std::fprintf(stderr, "[错误] 打开失败\n");
            if (!key.empty()) gryce_engine::resources::set_pak_crypto_key("");
            return 1;
        }
        for (const auto& e : r.entries()) {
            ++total;
            auto data = r.read(e.path);
            const uint32_t crc = gryce_engine::utils::crc32(data.data(), data.size());
            if (crc != e.crc32) {
                ++bad;
                std::printf("  [FAIL] %s（crc32 %08x != %08x）\n", e.path.c_str(), crc, e.crc32);
            }
        }
    }
    if (!key.empty()) gryce_engine::resources::set_pak_crypto_key("");
    if (bad == 0) {
        std::printf("[校验] 通过：%zu 个条目 CRC32 全部一致。\n", total);
        return 0;
    }
    std::printf("[校验] %zu/%zu 个条目 CRC 不匹配。\n", bad, total);
    return 1;
}

} // namespace

int cmd_pak(const Options& o) {
    const std::string sub = o.positionals.empty() ? "" : o.positionals.front();
    if (sub == "create") return pak_create(o);
    if (sub == "list") return pak_list(o);
    if (sub == "extract") return pak_extract(o);
    if (sub == "verify") return pak_verify(o);
    std::fprintf(stderr,
                 "[错误] pak 需要子命令：create | list | extract | verify\n"
                 "示例: GryceGC pak create --out assets.pak test/demo_project\n");
    return 2;
}

} // namespace grycegc
