#include "../cli.h"
#include "../core/dep_resolver.h"
#include "../core/fsutil.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <set>

namespace grycegc {

namespace {

std::filesystem::path resolve_exe(const std::filesystem::path& build_dir,
                                  const std::string& exe_arg) {
    if (!exe_arg.empty()) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(exe_arg, ec)) {
            return std::filesystem::absolute(exe_arg, ec);
        }
    }
    // 在 build_dir 内找 GryceRenderSmoke.exe
    if (build_dir.empty() || !std::filesystem::is_directory(build_dir)) return {};
    {
        std::error_code ec;
        auto top = build_dir / "GryceRenderSmoke.exe";
        if (std::filesystem::is_regular_file(top, ec)) return top;
    }
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(
             build_dir, std::filesystem::directory_options::skip_permission_denied, ec);
         it != std::filesystem::recursive_directory_iterator();
         it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (!it->is_regular_file(ec)) continue;
        const auto fn = it->path().filename().string();
        if (fn == "GryceRenderSmoke.exe" || fn == "grycerendersmoke.exe") {
            return it->path();
        }
    }
    return {};
}

} // namespace

int cmd_deps(const Options& opts) {
    const std::string build_dir_arg = opts.get("build-dir", "cmake-build-debug");
    const std::string exe_arg = opts.get("exe");
    std::filesystem::path build_dir = build_dir_arg;
    std::error_code ec;
    build_dir = std::filesystem::absolute(build_dir, ec);

    auto exe = resolve_exe(build_dir, exe_arg);
    if (exe.empty()) {
        std::fprintf(stderr, "[错误] 找不到可执行文件，请先用 --exe 指定。\n");
        return 2;
    }
    std::vector<std::filesystem::path> extra;
    extra.push_back(exe.parent_path());
    for (const auto& d : opts.get_all("extra-dir")) extra.emplace_back(d);
    for (const auto& d : discover_mingw_bins()) {
        if (std::find(extra.begin(), extra.end(), d) == extra.end()) extra.push_back(d);
    }
    DepResolver resolver(build_dir, extra);
    auto r = resolver.resolve(exe);

    std::printf("可执行文件: %s\n\n", exe.string().c_str());
    std::printf("依赖闭包:\n");
    std::vector<std::string> core, dep, miss;
    for (const auto& n : r.found) {
        (classify(n.name_lower) == DepCategory::Core ? core : dep)
            .push_back(n.name_orig);
    }
    miss = r.missing;
    std::sort(core.begin(), core.end());
    std::sort(dep.begin(), dep.end());
    std::sort(miss.begin(), miss.end());

    std::printf("  [core · 与 exe 同级]\n");
    for (const auto& n : core) std::printf("    %s\n", n.c_str());
    std::printf("  [dependency · 进 dll/]\n");
    for (const auto& n : dep) std::printf("    %s\n", n.c_str());
    if (!miss.empty()) {
        std::printf("  [未找到]\n");
        for (const auto& n : miss) std::printf("    %s\n", n.c_str());
    }
    return 0;
}

} // namespace grycegc
