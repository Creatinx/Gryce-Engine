// ---------------------------------------------------------------------------
// GryceGC run：跨平台启动器（替代 run*.bat）
//
// 用法:
//   GryceGC run --package <dir> [--api opengl|vulkan] [--play] [-- <exe args>]
//   GryceGC run --build-dir <dir> [同上]
//
// 行为：定位 exe → 把 <pkg>/dll 预置到 PATH 前面 → CreateProcess 启动 →
//       等待退出并原样传播退出码。
// ---------------------------------------------------------------------------

#include "../cli.h"
#include "../commands/commands.h"
#include "../core/fsutil.h"
#include "../core/process.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace grycegc {

namespace {

fs::path find_package_exe(const fs::path& pkg_root) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(pkg_root, ec)) {
        if (e.is_regular_file(ec) && e.path().extension() == ".exe") return e.path();
    }
    return {};
}

fs::path find_build_exe(const fs::path& build_dir) {
    const fs::path top = build_dir / "GryceRenderSmoke.exe";
    if (fs::exists(top)) return top;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(
             build_dir, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (it->is_regular_file(ec) && it->path().extension() == ".exe") return it->path();
    }
    return {};
}

} // namespace

int cmd_run(const Options& o) {
    fs::path pkg_root;
    std::string dll_folder = "dll";
    if (o.has("dll-folder")) dll_folder = o.get("dll-folder");

    if (o.has("package")) {
        pkg_root = o.get("package");
        if (!fs::is_directory(pkg_root)) {
            std::fprintf(stderr, "[错误] 包目录不存在：%s\n", pkg_root.string().c_str());
            return 2;
        }
    } else if (o.has("build-dir")) {
        pkg_root = o.get("build-dir");
        if (!fs::is_directory(pkg_root)) {
            std::fprintf(stderr, "[错误] 构建目录不存在：%s\n", pkg_root.string().c_str());
            return 2;
        }
    } else {
        std::fprintf(stderr, "[错误] 请指定 --package <dir> 或 --build-dir <dir>\n");
        return 2;
    }

    // 定位 exe
    fs::path exe;
    if (o.has("exe")) {
        exe = fs::absolute(o.get("exe"));
    } else {
        exe = (o.has("package")) ? find_package_exe(pkg_root) : find_build_exe(pkg_root);
    }
    if (exe.empty() || !fs::exists(exe)) {
        std::fprintf(stderr, "[错误] 未找到可执行文件（%s 内无 .exe）\n",
                     pkg_root.string().c_str());
        return 2;
    }

    std::string api = "opengl";
    if (o.has("api")) api = o.get("api");
    if (api != "opengl" && api != "vulkan") {
        std::fprintf(stderr, "[错误] --api 仅支持 opengl / vulkan\n");
        return 2;
    }

    // 组装 exe 参数：--api + 可选 --play + 透传参数
    std::vector<std::string> args;
    args.push_back("--api");
    args.push_back(api);
    if (o.is_flag("play")) args.push_back("--play");
    for (const auto& p : o.passthrough) args.push_back(p);

    // 环境：包/构建目录下的 dll/ 预置 PATH 前面
    const fs::path dll_dir = pkg_root / dll_folder;
    EnvOverrides env;
    if (fs::is_directory(dll_dir)) {
        const char* old_path = std::getenv("PATH");
        env.emplace_back("PATH", prepend_to_path_env(dll_dir.string(),
                                                     old_path ? old_path : ""));
    }

    std::printf("[run] %s", exe.string().c_str());
    for (const auto& a : args) std::printf(" %s", a.c_str());
    std::printf("\n");
    if (fs::is_directory(dll_dir))
        std::printf("[run] PATH 已注入: %s\n", dll_dir.string().c_str());

    const auto r = run_process(exe, args, env);
    if (!r.spawned) {
        std::fprintf(stderr, "[错误] 启动失败：%s\n", r.spawn_error.c_str());
        return 2;
    }
    return r.exit_code;
}

} // namespace grycegc
