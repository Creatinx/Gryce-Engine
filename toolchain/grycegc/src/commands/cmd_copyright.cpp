// ---------------------------------------------------------------------------
// GryceGC copyright：版权/许可印章（M2）
//
// 目标：
//   目录（发布包）  写 COPYRIGHT.txt + README.txt 页脚追加版权行 + 每个 PE 盖
//                   GRYCETL1 trailer（JSON payload）
//   单个 PE 文件    仅盖 trailer
//
// 幂等：重复执行结果不变（COPYRIGHT 全量重写；README 页脚去重；trailer 替换）。
// 用法:
//   GryceGC copyright --package <dir> --holder "..." [--year 2026]
//                       [--license MIT] [--license-file f] [--exe f ...]
//   GryceGC copyright --exe <file> --holder "..."
// ---------------------------------------------------------------------------

#include "../cli.h"
#include "../commands/commands.h"
#include "../core/fsutil.h"
#include "../core/meta.h"
#include "../core/pe_image.h"
#include "../core/versioninfo.h"

#include <cstdio>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace grycegc {

namespace {

constexpr const char* kCopyrightFooter = "[copyright]";

std::string build_copyright_text(const std::string& holder, const std::string& year,
                                 const std::string& license, const std::string& license_file) {
    std::string s;
    s += "Gryce-Engine\n";
    s += "================================================\n\n";
    s += "Copyright (c) " + year + " " + holder + "\n\n";
    if (!license.empty()) {
        s += "License: " + license + "\n";
        if (!license_file.empty()) {
            auto bytes = read_all(license_file);
            if (!bytes.empty()) {
                s += "\n------------------------------------------------\n";
                s.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                if (s.back() != '\n') s += "\n";
            }
        }
    } else if (!license_file.empty()) {
        auto bytes = read_all(license_file);
        if (!bytes.empty()) {
            s += "------------------------------------------------\n";
            s.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            if (s.back() != '\n') s += "\n";
        }
    }
    s += "\n由 GryceGC copyright 生成。\n";
    return s;
}

bool append_readme_footer(const fs::path& readme, const std::string& line) {
    if (!fs::exists(readme)) return true;  // 无 README 不强制创建
    auto bytes = read_all(readme);
    std::string s(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (s.find(kCopyrightFooter) != std::string::npos) return true;  // 已有页脚
    if (!s.empty() && s.back() != '\n') s += "\n";
    s += "\n" + std::string(kCopyrightFooter) + "\n" + line + "\n";
    return write_binary(readme, s);
}

// 对包根目录下的 exe + core DLL 盖 trailer（幂等：payload 固定时重复写无差异）
// 注意顺序：必须先写 VERSIONINFO 再写 trailer —— BeginUpdateResourceW 会重写
// 整个 PE 文件，后写的 EOF trailer 会被截掉。
bool stamp_pes(const fs::path& pkg_root, const std::string& payload_json,
               const std::string& product, const std::string& version,
               const std::string& one_line, bool with_version_info) {
    bool ok = true;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(pkg_root, ec)) {
        if (!e.is_regular_file(ec)) continue;
        const auto ext = e.path().extension().string();
        if (ext != ".exe" && ext != ".dll") continue;
        if (with_version_info) {
            VersionInfoFields vi;
            vi.company = payload_json.find("\"holder\"") != std::string::npos
                             ? nlohmann::json::parse(payload_json)["holder"].get<std::string>()
                             : "";
            vi.copyright = one_line;
            vi.product = product.empty() ? "Gryce-Engine" : product;
            vi.version = version.empty() ? "1.0.0" : version;
            vi.file_version = vi.version;
            if (!pe_write_version_info(e.path(), vi)) {
                std::fprintf(stderr, "[警告] VERSIONINFO 写入失败：%s\n",
                             e.path().string().c_str());
                ok = false;
            }
        }
        const nlohmann::json payload = nlohmann::json::parse(payload_json);
        if (!meta_write_pe_trailer(e.path(), payload)) {
            std::fprintf(stderr, "[警告] trailer 写入失败：%s\n", e.path().string().c_str());
            ok = false;
        }
    }
    return ok;
}

} // namespace

int cmd_copyright(const Options& o) {
    std::string holder = o.get("holder", "");
    if (holder.empty()) {
        std::fprintf(stderr, "[错误] 必须提供 --holder（版权持有者）\n");
        return 2;
    }
    std::string year = o.get("year", "2026");
    std::string license = o.get("license", "");
    std::string license_file = o.get("license-file", "");

    const std::string text = build_copyright_text(holder, year, license, license_file);
    const std::string one_line = "Copyright (c) " + year + " " + holder +
                                 (license.empty() ? "" : " — " + license);

    nlohmann::json payload;
    payload["gryce"] = "copyright";
    payload["holder"] = holder;
    payload["year"] = year;
    if (!license.empty()) payload["license"] = license;

    if (o.has("package")) {
        const fs::path pkg = o.get("package");
        if (!fs::is_directory(pkg)) {
            std::fprintf(stderr, "[错误] 包目录不存在：%s\n", pkg.string().c_str());
            return 2;
        }
        if (!write_binary(pkg / "COPYRIGHT.txt", text)) {
            std::fprintf(stderr, "[错误] 写 COPYRIGHT.txt 失败\n");
            return 2;
        }
        append_readme_footer(pkg / "README.txt", one_line);
        if (!stamp_pes(pkg, payload.dump(), o.get("product", ""), o.get("version", ""),
                       one_line, o.is_flag("version-info")))
            return 1;
        std::printf("[完成] 版权印章已写入 %s（COPYRIGHT.txt + README 页脚 + PE trailer%s）\n",
                    pkg.string().c_str(), o.is_flag("version-info") ? " + VERSIONINFO" : "");
        return 0;
    }

    if (o.has("exe")) {
        const fs::path exe = o.get("exe");
        if (!fs::exists(exe)) {
            std::fprintf(stderr, "[错误] 文件不存在：%s\n", exe.string().c_str());
            return 2;
        }
        if (!meta_write_pe_trailer(exe, payload)) {
            std::fprintf(stderr, "[错误] trailer 写入失败：%s\n", exe.string().c_str());
            return 1;
        }
        std::printf("[完成] 版权 trailer 已写入 %s\n", exe.string().c_str());
        return 0;
    }

    std::fprintf(stderr, "[错误] 请指定 --package <dir> 或 --exe <file>\n");
    return 2;
}

} // namespace grycegc
