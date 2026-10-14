#pragma once

// ---------------------------------------------------------------------------
// GryceGC packager：把构建目录整理成可脱线运行的分发目录
//
// 移植自 toolchain/resource.py 的 Packager，输出布局与 Python 版字节级一致：
//   <output>/<name>/
//     GryceRenderSmoke.exe    # exe
//     GryceCore.dll ...       # 引擎 core DLL，与 exe 同级
//     dll/                    # 第三方 + MinGW 运行时（run.bat 注入 PATH）
//     demo_project/           # 资源（--no-assets 可跳过）
//     run.bat / run_vulkan.bat / run_play.bat
//     README.txt
// ---------------------------------------------------------------------------

#include <string>
#include <vector>

#include "core/dep_resolver.h"

namespace fs = std::filesystem;

namespace grycegc {

struct PackageResult {
    bool ok = false;
    std::string error;
    fs::path pkg_root;
    std::string exe_name;                 // exe 文件名
    std::vector<std::string> core;        // 拷到包根的 core DLL（去重排序）
    std::vector<std::string> dep;         // 拷到 dll/ 的依赖 DLL（去重排序）
    std::vector<std::string> missing;     // 闭包内未找到的导入 DLL
    bool assets = false;                  // 是否打包了 demo_project
};

struct PackageOptions {
    fs::path build_dir = "cmake-build-debug";
    std::string exe;                      // 显式 exe（空 = 自动探测）
    fs::path output = "dist";
    std::string name = "GryceEngine";
    std::string project_src = "test/demo_project";  // 空 = 跳过
    std::string dll_folder = "dll";
    std::vector<fs::path> extra_dirs;
    bool no_assets = false;
    std::string api = "opengl";           // 默认启动器后端
    bool clean = false;
};

class Packager {
public:
    explicit Packager(const PackageOptions& opt) : opt_(opt) {}

    // 定位 exe：显式路径优先，其次 build_dir 内递归找 GryceRenderSmoke.exe
    fs::path resolve_exe() const;

    // 主流程；失败时 ok=false 且 error 非空
    PackageResult run() const;

    const PackageOptions& options() const { return opt_; }

private:
    void write_launchers(const fs::path& pkg_root, const std::string& exe_name) const;
    void write_readme(const fs::path& pkg_root, const std::string& exe_name,
                      const std::vector<std::string>& core,
                      const std::vector<std::string>& dep,
                      const std::vector<std::string>& missing, bool assets) const;

    PackageOptions opt_;
};

} // namespace grycegc
