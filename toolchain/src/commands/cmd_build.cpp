// ---------------------------------------------------------------------------
// GryceGC build：cmake configure + build 封装（M5）
//
// 用法:
//   GryceGC build [--build-dir build] [--preset debug|release]
//                 [--target GryceCore ...] [--d3d12 on|off]
//                 [--lto] [--test] [--clean] [--jobs N]
//                 [--cc <path>] [--cxx <path>]
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
    // 源码根：--source 优先；否则从 cwd 向上找含 CMakeLists.txt 的引擎根；再 fallback cwd。
    fs::path source_dir;
    if (o.has("source")) {
        source_dir = fs::absolute(o.get("source"));
    } else {
        source_dir = find_engine_root();
        if (source_dir.empty()) source_dir = fs::current_path();
    }
    // build-dir：相对路径以源码根为基（无论从引擎根还是其子目录调用都指向同一个 build/）。
    fs::path build_dir = o.has("build-dir") ? o.get("build-dir") : fs::path("build");
    build_dir = build_dir.is_absolute() ? fs::absolute(build_dir)
                                        : fs::absolute(source_dir / build_dir);

    // build 构建的是引擎本体（GryceApp / GryceRenderSmoke / 测试 / DLL），
    // 不是某个用户项目；--project 在这里无意义，给出提示避免误用。
    if (o.has("project")) {
        std::fprintf(stderr,
                     "[提示] build 构建的是引擎本体，--project 被忽略。"
                     " 用户项目通过 GryceApp --project <dir> 运行（见 run）。\n");
    }

    // 校验源码根确实含 CMakeLists.txt
    if (!fs::exists(source_dir / "CMakeLists.txt")) {
        std::fprintf(stderr,
                     "[错误] 构建源目录 %s 不含 CMakeLists.txt。\n"
                     "        build 命令构建的是引擎本体；请在引擎源码根目录运行，\n"
                     "        或用 --source <引擎源码根> 指定（例如 --source D:/Gryce-Engine）。\n",
                     source_dir.string().c_str());
        return 2;
    }

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
    std::vector<std::string> cfg = {"-S", source_dir.string(), "-B", build_dir.string(),
                                    "-G", "Ninja"};

    // 引擎只支持 MSYS2 UCRT64 的 g++，一律显式指定 MinGW 编译器——绝不依赖
    // cmake 在 cmd/PowerShell 下默认挑中的 MSVC。用户可用 --cc/--cxx 显式换。
    // 注意：对已配置过的目录，若缓存里记的是别的编译器，cmake 会报
    // "COMPILER was modified"——这正是我们想要的强提醒（该缓存目录本就不该用）。
    {
        const std::string cc = o.has("cc") ? o.get("cc")
                                           : find_tool("cc", {"D:/msys64/ucrt64/bin/cc.exe"});
        const std::string cxx = o.has("cxx") ? o.get("cxx")
                                             : find_tool("c++", {"D:/msys64/ucrt64/bin/c++.exe"});
        if (!cc.empty()) cfg.push_back("-DCMAKE_C_COMPILER=" + cc);
        if (!cxx.empty()) cfg.push_back("-DCMAKE_CXX_COMPILER=" + cxx);
        else {
            std::fprintf(stderr,
                         "[错误] 找不到 MSYS2 UCRT64 编译器（D:/msys64/ucrt64/bin/）。"
                         "请安装 MSYS2 或用 --cc/--cxx 显式指定。\n");
            return 2;
        }
    }
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
