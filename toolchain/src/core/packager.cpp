#include "core/packager.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <set>

#include "core/fsutil.h"
#include "core/pe_image.h"

namespace grycegc {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// 启动器正文。批处理变量用字面值 %HERE%（与 Python 版一致），
// 不写 C++ f-string 插值，避免 % 被吞。
std::string launcher_script(const std::string& exe_name, const std::string& api,
                            const std::string& extra) {
    std::string s;
    s += "@echo off\r\n";
    s += "setlocal\r\n";
    s += "set \"HERE=%~dp0\"\r\n";
    s += "set \"PATH=%HERE%dll;%PATH%\"\r\n";
    s += "\"%HERE%" + exe_name + "\" --project \"%HERE%demo_project\" --api " + api +
         " " + extra + "%*\r\n";
    s += "endlocal\r\n";
    return s;
}

} // namespace

fs::path Packager::resolve_exe() const {
    if (!opt_.exe.empty() && fs::exists(opt_.exe)) return fs::absolute(opt_.exe);
    if (!fs::is_directory(opt_.build_dir)) return {};
    const fs::path top = opt_.build_dir / "GryceRenderSmoke.exe";
    if (fs::exists(top)) return top;
    std::vector<std::pair<size_t, fs::path>> hits;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(
             opt_.build_dir, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (it->is_regular_file(ec) && lower(it->path().filename().string()) ==
                                          "grycerendersmoke.exe") {
            hits.emplace_back(it.depth(), it->path());
        }
    }
    if (hits.empty()) return {};
    std::sort(hits.begin(), hits.end());
    return hits.front().second;
}

PackageResult Packager::run() const {
    PackageResult res;
    const fs::path exe_path = resolve_exe();
    if (exe_path.empty()) {
        res.error = "找不到可执行文件（build_dir=" + opt_.build_dir.string() + ", exe=" +
                    opt_.exe + "）。请用 --exe 指定，或先构建工程。";
        return res;
    }

    // 依赖搜索路径：显式 extra + exe 同目录 + MinGW bin
    std::vector<fs::path> extra = opt_.extra_dirs;
    extra.push_back(exe_path.parent_path());
    for (const auto& d : discover_mingw_bins()) {
        if (std::find(extra.begin(), extra.end(), d) == extra.end()) extra.push_back(d);
    }

    DepResolver resolver(opt_.build_dir, extra);
    const auto closure = resolver.resolve(exe_path);

    const fs::path pkg_root = opt_.output / opt_.name;
    if (opt_.clean) fs::remove_all(pkg_root);
    mkdir_p(pkg_root);
    const fs::path dll_dir = pkg_root / opt_.dll_folder;
    mkdir_p(dll_dir);

    // exe 自身
    if (!grycegc::copy_file(exe_path, pkg_root / exe_path.filename())) {
        res.error = "拷贝 exe 失败：" + exe_path.string();
        return res;
    }
    res.exe_name = exe_path.filename().string();

    std::set<std::string> core_set, dep_set;
    for (const auto& node : closure.found) {
        if (node.source.empty()) continue;
        if (classify(node.name_lower) == DepCategory::Core) {
            if (!grycegc::copy_file(node.source, pkg_root / node.source.filename())) {
                res.error = "拷贝 core DLL 失败：" + node.source.string();
                return res;
            }
            core_set.insert(node.source.filename().string());
        } else {
            if (!grycegc::copy_file(node.source, dll_dir / node.source.filename())) {
                res.error = "拷贝依赖 DLL 失败：" + node.source.string();
                return res;
            }
            dep_set.insert(node.source.filename().string());
        }
    }
    res.core.assign(core_set.begin(), core_set.end());
    res.dep.assign(dep_set.begin(), dep_set.end());
    res.missing = closure.missing;
    std::sort(res.missing.begin(), res.missing.end());

    // 资源：整目录递归拷贝到 demo_project/
    if (!opt_.no_assets && !opt_.project_src.empty() && fs::is_directory(opt_.project_src)) {
        if (!copy_tree(opt_.project_src, pkg_root / "demo_project")) {
            res.error = "拷贝资源失败：" + opt_.project_src;
            return res;
        }
        res.assets = true;
    }

    write_launchers(pkg_root, res.exe_name);
    write_readme(pkg_root, res.exe_name, res.core, res.dep, res.missing, res.assets);

    res.pkg_root = pkg_root;
    res.ok = true;
    return res;
}

void Packager::write_launchers(const fs::path& pkg_root,
                               const std::string& exe_name) const {
    write_binary(pkg_root / "run.bat", launcher_script(exe_name, "opengl", ""));
    write_binary(pkg_root / "run_vulkan.bat", launcher_script(exe_name, "vulkan", ""));
    write_binary(pkg_root / "run_play.bat", launcher_script(exe_name, "opengl", "--play "));
}

void Packager::write_readme(const fs::path& pkg_root, const std::string& exe_name,
                            const std::vector<std::string>& core,
                            const std::vector<std::string>& dep,
                            const std::vector<std::string>& missing, bool assets) const {
    std::string s;
    s += "Gryce-Engine 离线发布包：" + opt_.name + "\n";
    s += "================================================\n\n";
    s += "本文件夹可脱离构建环境（无需 MinGW / MSYS2 / PATH 配置）直接运行。\n\n";
    s += "运行方式（二选一）：\n";
    s += "  1) 双击 run.bat         —— OpenGL 后端（默认，最稳）\n";
    s += "  2) 双击 run_vulkan.bat   —— Vulkan 后端（目标机需装有 Vulkan 驱动）\n";
    s += "     run_play.bat          —— 交互模式（WASD + 鼠标）\n\n";
    s += "也可以命令行加参数，例如：\n";
    s += "  run.bat --frames 120 --shot out.png\n\n";
    s += "目录结构\n";
    s += "------------------------------------------------\n";
    s += opt_.name + "/\n";
    s += "  " + exe_name + "   (可执行文件)\n";
    for (const auto& c : core) s += "  " + c + "   (引擎 core DLL，与 exe 同级)\n";
    s += "  " + opt_.dll_folder + "/\n";
    for (const auto& d : dep) s += "    " + d + "   (依赖 DLL，由 run.bat 注入 PATH)\n";
    if (assets) s += "  demo_project/   (资源：models / scenes / shaders)\n";
    s += "  run.bat / run_vulkan.bat / run_play.bat\n\n";
    s += "为什么能脱机运行\n";
    s += "------------------------------------------------\n";
    s += "- 引擎自身 DLL 放在 exe 同目录，OS 优先从 exe 目录加载；\n";
    s += "- 其余第三方 + MinGW 运行时放进 " + opt_.dll_folder + "/，run.bat 启动前\n";
    s += "  将其预置进 PATH，Windows 在 exe 目录之后搜索 PATH 找到它们；\n";
    s += "- 资源通过 --project 指向包内 demo_project，不依赖源码树路径。\n\n";
    if (!missing.empty()) {
        s += "注意：以下导入 DLL 在本次打包时未能在构建树中找到，\n";
        s += "可能在目标机缺失（若是系统 DLL 则无碍，否则需手动补齐）：\n";
        for (const auto& m : missing) s += "  - " + m + "\n";
        s += "\n";
    }
    s += "由 GryceGC package 生成（Gryce-Engine 工具链）。\n";
    // Windows 文本文件惯例 CRLF（与 Python 版 output 一致）
    std::string crlf;
    crlf.reserve(s.size() + s.size() / 20);
    for (char c : s) {
        if (c == '\n') crlf += "\r\n";
        else crlf += c;
    }
    write_binary(pkg_root / "README.txt", crlf);
}

} // namespace grycegc
