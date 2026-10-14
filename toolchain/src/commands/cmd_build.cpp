// ---------------------------------------------------------------------------
// GryceGC build：cmake configure + build 封装（M5）
//
// 用法:
//   GryceGC build [--build-dir build] [--preset debug|release]
//                 [--target GryceCore ...] [--d3d12 on|off]
//                 [--lto] [--test] [--clean] [--jobs N]
//
// 复用主工程同一套工具链：cmake + Ninja（PATH 或 CLion 自带兜底）。
// ---------------------------------------------------------------------------

#include "../cli.h"
#include "../commands/commands.h"
#include "../core/fsutil.h"
#include "../core/process.h"

#include <cstdio>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace grycegc {

namespace {

std::string find_tool(const std::string& name, const std::vector<std::string>& fallbacks) {
    if (auto p = which(name); !p.empty()) return p;
    for (const auto& f : fallbacks)
        if (fs::exists(f)) return f;
    return {};
}

} // namespace

int cmd_build(const Options& o) {
    fs::path build_dir = o.has("build-dir") ? o.get("build-dir") : fs::path("build");
    const std::string preset = o.get("preset", "debug");

    const std::string cmake = find_tool("cmake", {
        "D:/JetBrains/CLion 2026.1/bin/cmake/win/x64/bin/cmake.exe",
    });
    const std::string ninja = find_tool("ninja", {
        "D:/JetBrains/CLion 2026.1/bin/ninja/win/x64/ninja.exe",
    });
    if (cmake.empty()) {
        std::fprintf(stderr, "[错误] 找不到 cmake（PATH 或 CLion 自带）\n");
        return 2;
    }
    if (ninja.empty()) {
        std::fprintf(stderr, "[错误] 找不到 ninja（PATH 或 CLion 自带）\n");
        return 2;
    }

    if (o.is_flag("clean")) {
        std::printf("[build] 清空 %s\n", build_dir.string().c_str());
        fs::remove_all(build_dir);
    }

    // --- configure -----------------------------------------------------------
    std::vector<std::string> cfg = {"-S", ".", "-B", build_dir.string(), "-G", "Ninja"};
    cfg.push_back("-DCMAKE_MAKE_PROGRAM=" + ninja);
    cfg.push_back(std::string("-DCMAKE_BUILD_TYPE=") +
                  (preset == "release" ? "Release" : "Debug"));
    const std::string d3d12 = o.get("d3d12", "on");
    cfg.push_back(std::string("-DGRYCE_ENABLE_D3D12=") + (d3d12 == "off" ? "OFF" : "ON"));
    if (o.is_flag("lto")) cfg.push_back("-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON");

    std::printf("[build] configure: %s\n", cmake.c_str());
    for (const auto& a : cfg) std::printf("    %s\n", a.c_str());
    auto cr = run_process(cmake, cfg);
    if (!cr.spawned || cr.exit_code != 0) {
        std::fprintf(stderr, "[错误] cmake configure 失败（exit=%d）\n", cr.exit_code);
        return 1;
    }

    // --- build ---------------------------------------------------------------
    std::vector<std::string> bld = {"--build", build_dir.string(), "--parallel"};
    const int jobs = o.get_int("jobs", 0);
    if (jobs > 0) bld.push_back(std::to_string(jobs));
    for (const auto& t : o.get_all("target")) {
        bld.push_back("--target");
        bld.push_back(t);
    }
    std::printf("[build] building...\n");
    auto br = run_process(cmake, bld);
    if (!br.spawned || br.exit_code != 0) {
        std::fprintf(stderr, "[错误] 构建失败（exit=%d）\n", br.exit_code);
        return 1;
    }

    // --- test ----------------------------------------------------------------
    if (o.is_flag("test")) {
        std::vector<std::string> tst = {"--build", build_dir.string(), "--target", "test"};
        auto tr = run_process(cmake, tst);
        if (!tr.spawned || tr.exit_code != 0) {
            std::fprintf(stderr, "[错误] CTest 失败（exit=%d）\n", tr.exit_code);
            return 1;
        }
    }

    std::printf("[完成] 构建成功：%s\n", build_dir.string().c_str());
    return 0;
}

} // namespace grycegc
