// ---------------------------------------------------------------------------
// GryceGC package：打包成可脱线运行的发布目录（M2，移植自 toolchain/main.py）
// ---------------------------------------------------------------------------

#include "../cli.h"
#include "../commands/commands.h"
#include "../core/fsutil.h"
#include "../core/packager.h"
#include "../core/pe_image.h"

#include <cstdio>
#include <string>

namespace fs = std::filesystem;

namespace grycegc {

namespace {

// 从 project.data 里粗略读 render_api（不引 JSON 库，按行找 "render_api" 后的引号串）。
std::string read_render_api(const fs::path& project_src) {
    if (!fs::is_directory(project_src)) return {};
    const auto buf = read_all(project_src / "project.data");
    if (buf.empty()) return {};
    const std::string s(reinterpret_cast<const char*>(buf.data()), buf.size());
    const auto pos = s.find("\"render_api\"");
    if (pos == std::string::npos) return {};
    const auto colon = s.find(':', pos);
    if (colon == std::string::npos) return {};
    const auto q1 = s.find('"', colon);
    if (q1 == std::string::npos) return {};
    const auto q2 = s.find('"', q1 + 1);
    if (q2 == std::string::npos) return {};
    return s.substr(q1 + 1, q2 - q1 - 1);
}

int verify_package_dir(const fs::path& pkg_root, const std::string& dll_folder) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(pkg_root)) {
        std::fprintf(stderr, "[错误] 包目录不存在：%s\n", pkg_root.string().c_str());
        return 2;
    }
    // 包内 exe：取第一个 .exe（与 Python 版一致）
    fs::path exe;
    for (const auto& e : fs::directory_iterator(pkg_root, ec)) {
        if (e.is_regular_file(ec) && e.path().extension() == ".exe") { exe = e.path(); break; }
    }
    if (exe.empty()) {
        std::printf("[校验] 包内未找到 .exe\n");
        return 1;
    }
    const fs::path dll_dir = pkg_root / dll_folder;
    DepResolver resolver(pkg_root, {pkg_root, dll_dir});
    const auto closure = resolver.resolve(exe);

    std::vector<std::string> problems;
    for (const auto& m : closure.missing) problems.push_back("包内缺失导入 DLL: " + m);
    for (const auto& node : closure.found) {
        const auto bucket = classify(node.name_lower);
        const fs::path dir = node.source.parent_path();
        const fs::path want = (bucket == DepCategory::Core) ? pkg_root : dll_dir;
        if (std::error_code eq; dir != want && !fs::equivalent(dir, want, eq)) {
            problems.push_back(node.source.filename().string() +
                               (bucket == DepCategory::Core ? " 应放在 exe 同级（" : " 应放在 " +
                               dll_folder + "/（") + want.string() + "）");
        }
    }
    if (problems.empty()) {
        std::printf("[校验] 通过：所有导入 DLL 均可在包内解析。\n");
        return 0;
    }
    std::printf("[校验] 存在问题：\n");
    for (const auto& p : problems) std::printf("    - %s\n", p.c_str());
    return 1;
}

} // namespace

int cmd_package(const Options& o) {
    PackageOptions po;
    if (o.has("build-dir")) {
        po.build_dir = o.get("build-dir");
    } else {
        // 缺省自动探测：build → cmake-build-debug → cmake-build-release，
        // 优先含 GryceApp.exe 的，别再让人对着默认名报错。
        po.build_dir = find_build_dir({"build", "cmake-build-debug", "cmake-build-release"});
        std::printf("[package] 未指定 --build-dir，自动探测：%s\n", po.build_dir.string().c_str());
    }
    if (o.has("exe")) po.exe = o.get("exe");
    if (o.has("output")) po.output = o.get("output");
    if (o.has("name")) po.name = o.get("name");
    if (o.has("project")) po.project_src = o.get("project");
    if (o.has("dll-folder")) po.dll_folder = o.get("dll-folder");
    for (const auto& d : o.get_all("extra-dir")) po.extra_dirs.emplace_back(d);
    if (o.is_flag("no-assets")) po.no_assets = true;
    if (o.is_flag("clean")) po.clean = true;

    // build-dir：相对路径以引擎根为基，这样从引擎任意子目录调用 --build-dir build
    // 都指向同一个 build/（与 build / run 的基准一致）。
    if (!po.build_dir.is_absolute()) {
        const fs::path root = find_engine_root();
        po.build_dir = (root.empty() ? fs::current_path() : root) / po.build_dir;
    }
    po.build_dir = fs::absolute(po.build_dir);

    // --api 缺省时读 project.data 的 render_api；dx12 离线不支持，回落到 opengl。
    // 显式给出则尊重用户选择（仍限定 opengl / vulkan，dx12 离线包不保证 DLL 齐备）。
    if (o.has("api")) {
        po.api = o.get("api");
    } else {
        const std::string ra = read_render_api(po.project_src);
        po.api = (ra == "vulkan") ? "vulkan" : "opengl";
    }
    if (po.api != "opengl" && po.api != "vulkan") {
        std::fprintf(stderr, "[错误] 离线包 --api 仅支持 opengl / vulkan（当前 %s）\n",
                     po.api.c_str());
        return 2;
    }
    if (!fs::is_directory(po.build_dir)) {
        std::fprintf(stderr, "[错误] 构建目录不存在：%s\n", po.build_dir.string().c_str());
        return 2;
    }

    Packager packager(po);
    const auto res = packager.run();
    if (!res.ok) {
        std::fprintf(stderr, "[错误] %s\n", res.error.c_str());
        return 2;
    }

    std::printf("[完成] 发布包已生成：%s\n", res.pkg_root.string().c_str());
    std::printf("  可执行文件   : %s\n", res.exe_name.c_str());
    std::printf("  core DLL(同级): ");
    for (size_t i = 0; i < res.core.size(); ++i)
        std::printf("%s%s", i ? ", " : "", res.core[i].c_str());
    if (res.core.empty()) std::printf("(无)");
    std::printf("\n");
    std::printf("  依赖 DLL(%s/): ", po.dll_folder.c_str());
    for (size_t i = 0; i < res.dep.size(); ++i)
        std::printf("%s%s", i ? ", " : "", res.dep[i].c_str());
    if (res.dep.empty()) std::printf("(无)");
    std::printf("\n");
    std::printf("  资源 demo_project: %s\n", res.assets ? "已打包" : "跳过");
    if (!res.missing.empty()) {
        std::printf("  [警告] 未找到的导入 DLL: ");
        for (size_t i = 0; i < res.missing.size(); ++i)
            std::printf("%s%s", i ? ", " : "", res.missing[i].c_str());
        std::printf("\n");
    }

    if (o.is_flag("verify")) return verify_package_dir(res.pkg_root, po.dll_folder);
    return 0;
}

} // namespace grycegc
