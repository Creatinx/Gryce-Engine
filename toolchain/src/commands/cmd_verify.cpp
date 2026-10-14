#include "../cli.h"
#include "../core/dep_resolver.h"
#include "../core/fsutil.h"

#include <cstdio>
#include <filesystem>

namespace grycegc {

int cmd_verify(const Options& opts) {
    const std::string pkg_arg = opts.get("package");
    if (pkg_arg.empty()) {
        std::fprintf(stderr, "[错误] --package 必填\n");
        return 2;
    }
    const std::string dll_folder = opts.get("dll-folder", "dll");

    std::error_code ec;
    std::filesystem::path pkg = std::filesystem::absolute(pkg_arg, ec);
    if (!std::filesystem::is_directory(pkg, ec)) {
        std::fprintf(stderr, "[错误] 包目录不存在：%s\n", pkg.string().c_str());
        return 2;
    }

    // 在 pkg_root 找 .exe
    std::filesystem::path exe;
    for (auto& e : std::filesystem::directory_iterator(pkg, ec)) {
        if (ec) continue;
        if (!e.is_regular_file(ec)) continue;
        const auto fn = e.path().filename().string();
        if (fn.size() > 4 &&
            (fn.substr(fn.size() - 4) == ".exe" || fn.substr(fn.size() - 4) == ".EXE")) {
            exe = e.path();
            break;
        }
    }
    if (exe.empty()) {
        std::fprintf(stderr, "[校验] 失败：包内未找到 .exe\n");
        return 1;
    }

    std::filesystem::path dll_dir = pkg / dll_folder;
    DepResolver resolver(pkg, std::vector<std::filesystem::path>{pkg, dll_dir});
    auto r = resolver.resolve(exe);

    std::vector<std::string> problems;
    if (!r.missing.empty()) {
        std::string s = "包内缺失以下导入 DLL：";
        for (size_t i = 0; i < r.missing.size(); ++i) {
            if (i) s += ", ";
            s += r.missing[i];
        }
        problems.push_back(s);
    }
    // core 必须与 exe 同级；依赖必须在 dll_dir
    const std::string pkg_norm = std::filesystem::weakly_canonical(pkg).string();
    const std::string dll_norm = std::filesystem::weakly_canonical(dll_dir).string();
    for (const auto& n : r.found) {
        std::error_code ec2;
        const auto src = std::filesystem::weakly_canonical(n.source, ec2).string();
        const auto cat = classify(n.name_lower);
        if (cat == DepCategory::Core && src != pkg_norm) {
            problems.push_back(n.name_orig + " 应放在 exe 同级（" + pkg_norm + "）");
        }
        if (cat == DepCategory::Dependency && src != dll_norm) {
            problems.push_back(n.name_orig + " 应放在 " + dll_folder + "/");
        }
    }
    if (problems.empty()) {
        std::printf("[校验] 通过：%s 自洽。\n", pkg_norm.c_str());
        return 0;
    }
    std::fprintf(stderr, "[校验] %s 存在问题：\n", pkg_norm.c_str());
    for (const auto& p : problems) std::fprintf(stderr, "    - %s\n", p.c_str());
    return 1;
}

} // namespace grycegc
