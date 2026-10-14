// ---------------------------------------------------------------------------
// GryceGC meta：元数据读写（M2）
//
// 用法:
//   GryceGC meta set --key k --value v --target dir|pe-file [--project]
//   GryceGC meta get --key k [--target dir|pe-file] [--project]
//   GryceGC meta show [--target dir|pe-file] [--project]
//   GryceGC meta stamp-git --target dir   # M5：写入 git commit 戳
//
// 目标语义：
//   --target 是目录      → gryce.meta.json（合并写）
//   --target 是 PE 文件  → EOF trailer JSON（合并写）
//   --project            → 合并进 <target>/project.data（k=v）
// ---------------------------------------------------------------------------

#include "../cli.h"
#include "../commands/commands.h"
#include "../core/fsutil.h"
#include "../core/gitinfo.h"
#include "../core/meta.h"

#include <cstdio>
#include <string>

namespace fs = std::filesystem;

namespace grycegc {

namespace {

int meta_stamp_git(const Options& o) {
    const std::string target = o.get("target", ".");
    const fs::path tp(target);
    const auto st = git_stamp();
    if (!st.ok) {
        std::fprintf(stderr, "[错误] 取 git HEAD 失败（不在 git 仓库？）\n");
        return 1;
    }
    const std::string short_sha = st.commit.substr(0, 12);
    const std::string built_at = st.date.empty() ? now_iso8601() : st.date;

    std::error_code ec;
    if (fs::is_regular_file(tp, ec)) {
        auto j = meta_read_pe_trailer(tp).value_or(nlohmann::json::object());
        if (!j.is_object()) j = nlohmann::json::object();
        j["build_id"] = short_sha;
        j["built_at"] = built_at;
        if (!meta_write_pe_trailer(tp, j)) {
            std::fprintf(stderr, "[错误] 写 PE trailer 失败\n");
            return 1;
        }
        std::printf("[完成] %s trailer build_id=%s built_at=%s\n", target.c_str(),
                    short_sha.c_str(), built_at.c_str());
        return 0;
    }

    MetaDoc doc = meta_read_dir(tp);
    doc.build_id = short_sha;
    doc.built_at = built_at;
    if (!meta_write_dir(tp, doc)) {
        std::fprintf(stderr, "[错误] 写 gryce.meta.json 失败\n");
        return 1;
    }
    std::printf("[完成] %s/gryce.meta.json build_id=%s built_at=%s\n", target.c_str(),
                short_sha.c_str(), built_at.c_str());
    return 0;
}

int meta_set(const Options& o) {
    const std::string key = o.get("key", "");
    if (key.empty()) {
        std::fprintf(stderr, "[错误] meta set 需要 --key\n");
        return 2;
    }
    if (!o.has("value")) {
        std::fprintf(stderr, "[错误] meta set 需要 --value\n");
        return 2;
    }
    const std::string value = o.get("value");
    const std::string target = o.get("target", ".");
    const fs::path tp(target);

    if (o.is_flag("project")) {
        if (!fs::is_directory(tp)) {
            std::fprintf(stderr, "[错误] --project 目标必须是目录：%s\n", target.c_str());
            return 2;
        }
        if (!meta_merge_project_data(tp, {{key, value}})) {
            std::fprintf(stderr, "[错误] 写 project.data 失败\n");
            return 1;
        }
        std::printf("[完成] project.data %s=%s\n", key.c_str(), value.c_str());
        return 0;
    }

    std::error_code ec;
    if (fs::is_regular_file(tp, ec)) {
        // PE trailer 合并
        auto existing = meta_read_pe_trailer(tp).value_or(nlohmann::json::object());
        if (!existing.is_object()) existing = nlohmann::json::object();
        existing[key] = value;
        if (!meta_write_pe_trailer(tp, existing)) {
            std::fprintf(stderr, "[错误] 写 PE trailer 失败：%s\n", target.c_str());
            return 1;
        }
        std::printf("[完成] %s trailer %s=%s\n", target.c_str(), key.c_str(), value.c_str());
        return 0;
    }

    // 目录：gryce.meta.json 合并
    MetaDoc doc = meta_read_dir(tp);
    nlohmann::json j = doc.to_json();
    if (!j.is_object()) j = nlohmann::json::object();
    j[key] = value;
    MetaDoc merged = MetaDoc::from_json(j);
    if (!meta_write_dir(tp, merged)) {
        std::fprintf(stderr, "[错误] 写 gryce.meta.json 失败\n");
        return 1;
    }
    std::printf("[完成] %s/gryce.meta.json %s=%s\n", target.c_str(), key.c_str(),
                value.c_str());
    return 0;
}

int meta_get(const Options& o) {
    const std::string key = o.get("key", "");
    if (key.empty()) {
        std::fprintf(stderr, "[错误] meta get 需要 --key\n");
        return 2;
    }
    const std::string target = o.get("target", ".");
    const fs::path tp(target);

    if (o.is_flag("project")) {
        const auto v = project_data_get(tp, key);
        if (!v) {
            std::fprintf(stderr, "[错误] project.data 中不存在键 %s\n", key.c_str());
            return 1;
        }
        std::printf("%s\n", v->c_str());
        return 0;
    }

    std::error_code ec;
    if (fs::is_regular_file(tp, ec)) {
        const auto j = meta_read_pe_trailer(tp);
        if (!j || !j->contains(key)) {
            std::fprintf(stderr, "[错误] trailer 中不存在键 %s\n", key.c_str());
            return 1;
        }
        const auto& v = (*j)[key];
        std::printf("%s\n", v.is_string() ? v.get<std::string>().c_str() : v.dump().c_str());
        return 0;
    }

    const auto j = meta_read_dir(tp).to_json();
    if (!j.contains(key)) {
        std::fprintf(stderr, "[错误] gryce.meta.json 中不存在键 %s\n", key.c_str());
        return 1;
    }
    const auto& v = j[key];
    std::printf("%s\n", v.is_string() ? v.get<std::string>().c_str() : v.dump().c_str());
    return 0;
}

int meta_show(const Options& o) {
    const std::string target = o.get("target", ".");
    const fs::path tp(target);

    if (o.is_flag("project")) {
        std::ifstream f(tp / "project.data", std::ios::binary);
        if (!f) {
            std::fprintf(stderr, "[错误] 无 project.data：%s\n", target.c_str());
            return 2;
        }
        std::printf("%s", std::string(std::istreambuf_iterator<char>(f),
                                      std::istreambuf_iterator<char>()).c_str());
        return 0;
    }

    std::error_code ec;
    if (fs::is_regular_file(tp, ec)) {
        const auto j = meta_read_pe_trailer(tp);
        if (!j) {
            std::fprintf(stderr, "[错误] 无 PE trailer：%s\n", target.c_str());
            return 1;
        }
        std::printf("%s\n", j->dump(2).c_str());
        return 0;
    }

    const auto j = meta_read_dir(tp).to_json();
    if (j.empty()) {
        std::fprintf(stderr, "[错误] 无 gryce.meta.json：%s\n", target.c_str());
        return 1;
    }
    std::printf("%s\n", j.dump(2).c_str());
    return 0;
}

} // namespace

int cmd_meta(const Options& o) {
    // 子命令放在位置参数第一个
    std::string sub = o.positionals.empty() ? "" : o.positionals.front();
    if (sub == "set") return meta_set(o);
    if (sub == "get") return meta_get(o);
    if (sub == "show") return meta_show(o);
    if (sub == "stamp-git") return meta_stamp_git(o);
    std::fprintf(stderr,
                 "[错误] meta 需要子命令：set | get | show | stamp-git\n"
                 "示例: GryceGC meta set --key version --value 1.2.3 --target dist/GryceEngine\n");
    return 2;
}

} // namespace grycegc
