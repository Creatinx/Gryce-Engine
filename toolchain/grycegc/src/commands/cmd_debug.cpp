// ---------------------------------------------------------------------------
// GryceGC debug：以预设调试环境变量启动宿主（M4）
//
// 把散落在文档/记忆里的 GRYCE_* 调试开关固化为 CLI 预设：
//   --d3d12            GRYCE_D3D12_DEBUG=1            D3D12 debug layer
//   --gpu-validation   GRYCE_D3D12_GPU_VALIDATION=1   GPU-based validation
//   --serialize        GRYCE_D3D12_SERIALIZE=1
//   --trace-hdr        GRYCE_TRACE_HDR=1
//   --trace-frame N    GRYCE_DX12_TRACE_FRAME=N       仅追踪第 N 帧
//   --trace-bind       GRYCE_DX12_TRACE_BIND=1
//   --probe-ssr        GRYCE_DX12_PROBE_SSR=1
//   --probe-frame N    GRYCE_DX12_PROBE_FRAME=N
//   --probe-cbv NAME   GRYCE_DX12_PROBE_CBV=NAME
//   --ring             GRYCE_DX12_PROBE_RING=1
//   --glog N           GLOG_minloglevel=N             日志级别（0=最吵）
//   --with-debugger D  cdb / windbg / x64dbg（挂调试器启动）
//   --api A            --api opengl|vulkan|dx12
//   --package/--build-dir/--exe  启动目标（同 run）
//   --                 透传给宿主的参数
// ---------------------------------------------------------------------------

#include "../cli.h"
#include "../commands/commands.h"
#include "../core/process.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace grycegc {

namespace {

fs::path find_any_exe(const fs::path& root) {
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(
             root, fs::directory_options::skip_permission_denied, ec)) {
        if (ec) { ec.clear(); continue; }
        if (e.is_regular_file(ec) && e.path().extension() == ".exe" &&
            e.path().filename() != "GryceGC.exe")
            return e.path();
    }
    return {};
}

} // namespace

int cmd_debug(const Options& o) {
    fs::path root;
    if (o.has("package")) root = o.get("package");
    else if (o.has("build-dir")) root = o.get("build-dir");
    else if (o.has("exe")) root = fs::absolute(o.get("exe"));
    else {
        std::fprintf(stderr, "[错误] 请指定 --package/--build-dir/--exe\n");
        return 2;
    }

    fs::path exe;
    if (o.has("exe")) {
        exe = fs::absolute(o.get("exe"));
    } else {
        exe = (o.has("package")) ? find_any_exe(root) : find_any_exe(root);
    }
    if (exe.empty() || !fs::exists(exe)) {
        std::fprintf(stderr, "[错误] 未找到可执行文件（%s）\n", root.string().c_str());
        return 2;
    }

    EnvOverrides env;
    auto add = [&](const char* name, const std::string& value) {
        env.emplace_back(name, value);
        std::printf("[debug] %s=%s\n", name, value.c_str());
    };
    if (o.is_flag("d3d12")) add("GRYCE_D3D12_DEBUG", "1");
    if (o.is_flag("gpu-validation")) add("GRYCE_D3D12_GPU_VALIDATION", "1");
    if (o.is_flag("serialize")) add("GRYCE_D3D12_SERIALIZE", "1");
    if (o.is_flag("trace-hdr")) add("GRYCE_TRACE_HDR", "1");
    if (o.has("trace-frame")) add("GRYCE_DX12_TRACE_FRAME", o.get("trace-frame"));
    if (o.is_flag("trace-bind")) add("GRYCE_DX12_TRACE_BIND", "1");
    if (o.is_flag("probe-ssr")) add("GRYCE_DX12_PROBE_SSR", "1");
    if (o.has("probe-frame")) add("GRYCE_DX12_PROBE_FRAME", o.get("probe-frame"));
    if (o.has("probe-cbv")) add("GRYCE_DX12_PROBE_CBV", o.get("probe-cbv"));
    if (o.is_flag("ring")) add("GRYCE_DX12_PROBE_RING", "1");
    if (o.has("glog")) add("GLOG_minloglevel", o.get("glog"));

    std::vector<std::string> args;
    if (o.has("api")) { args.push_back("--api"); args.push_back(o.get("api")); }
    for (const auto& p : o.passthrough) args.push_back(p);

    // 包目录：注入 dll/ 到 PATH（与 run 一致）
    if (o.has("package")) {
        const fs::path dll_dir = root / "dll";
        if (fs::is_directory(dll_dir)) {
            const char* old = std::getenv("PATH");
            env.emplace_back("PATH", prepend_to_path_env(dll_dir.string(), old ? old : ""));
        }
    }

    // 挂调试器：debugger [args] -- target target-args
    fs::path spawn_exe = exe;
    std::vector<std::string> spawn_args = args;
    if (o.has("with-debugger")) {
        const std::string dbg = o.get("with-debugger");
        std::vector<std::string> pre;
        if (dbg == "cdb") pre = {"cdb.exe"};
        else if (dbg == "windbg") pre = {"windbg.exe"};
        else if (dbg == "x64dbg") pre = {"x64dbg.exe"};
        else {
            std::fprintf(stderr, "[错误] --with-debugger 仅支持 cdb / windbg / x64dbg\n");
            return 2;
        }
        if (dbg == "x64dbg") {
            pre.push_back("/C");
            pre.push_back("\"" + exe.string() + "\"");
            for (const auto& a : args) pre.push_back(a);
            spawn_args = pre;
        } else {
            pre.push_back("-g");
            pre.push_back("-o");
            pre.push_back(exe.string());
            for (const auto& a : args) pre.push_back(a);
            spawn_args = pre;
        }
        spawn_exe = pre.front();
        std::printf("[debug] 挂载调试器: %s\n", dbg.c_str());
    }

    const auto r = run_process(spawn_exe, spawn_args, env);
    if (!r.spawned) {
        std::fprintf(stderr, "[错误] 启动失败：%s\n", r.spawn_error.c_str());
        return 2;
    }
    return r.exit_code;
}

} // namespace grycegc
