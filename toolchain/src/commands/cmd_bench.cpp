// ---------------------------------------------------------------------------
// GryceGC bench：帧耗时 A/B 基准（M4，移植自 toolchain/bench_ab.sh）
//
// 用法:
//   GryceGC bench --build-dir build --w 1280 --h 720 [--runs 3] [--api opengl]
//
// 行为：对 GryceRenderSmoke.exe 跑 N 轮 --bench 240，解析 stdout 中的
//   「帧耗时」「分相平均」「gpu] 合计」行，输出单行 KV（grep 友好）。
// 注意：glog 会把 [2026/10/06/14/26/34] 片段插进 stdout，先剥离再解析。
// ---------------------------------------------------------------------------

#include "../cli.h"
#include "../commands/commands.h"
#include "../core/fsutil.h"
#include "../core/process.h"

#include <cstdio>
#include <regex>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace grycegc {

namespace {

fs::path find_bench_exe(const fs::path& build_dir) {
    const fs::path top = build_dir / "GryceRenderSmoke.exe";
    if (fs::exists(top)) return top;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(
             build_dir, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (it->is_regular_file(ec) && it->path().filename() == "GryceRenderSmoke.exe")
            return it->path();
    }
    return {};
}

std::string strip_glog_stamps(const std::string& s) {
    static const std::regex re(R"(\[\d+/\d+/\d+/\d+/\d+/\d+\])");
    return std::regex_replace(s, re, "");
}

// 提取 key 之后的第一个数值（跳过 "="、"："、中文说明等非数字字符）
std::string first_num(const std::string& line, const std::string& key) {
    const size_t p = line.find(key);
    if (p == std::string::npos) return "";
    size_t i = p + key.size();
    while (i < line.size() && !std::isdigit(static_cast<unsigned char>(line[i])) &&
           line[i] != '.')
        ++i;
    size_t j = i;
    while (j < line.size() && (std::isdigit(static_cast<unsigned char>(line[j])) ||
                               line[j] == '.'))
        ++j;
    return line.substr(i, j - i);
}

// 提取 "(370.7 FPS)" 形式：数字在关键词之前
std::string fps_num(const std::string& line) {
    const size_t p = line.find("FPS");
    if (p == std::string::npos) return "";
    size_t i = p;
    while (i > 0 && line[i - 1] == ' ') --i;
    size_t j = i;
    while (j > 0 && (std::isdigit(static_cast<unsigned char>(line[j - 1])) ||
                     line[j - 1] == '.'))
        --j;
    return line.substr(j, i - j);
}

} // namespace

int cmd_bench(const Options& o) {
    fs::path build_dir = o.has("build-dir") ? o.get("build-dir") : fs::path("build");
    const int w = o.get_int("w", 1280);
    const int h = o.get_int("h", 720);
    const int runs = o.get_int("runs", 3);
    const std::string api = o.get("api", "opengl");

    const fs::path exe = find_bench_exe(build_dir);
    if (exe.empty()) {
        std::fprintf(stderr, "[错误] 找不到 GryceRenderSmoke.exe（%s）\n",
                     build_dir.string().c_str());
        return 2;
    }

    for (int r = 1; r <= runs; ++r) {
        const std::vector<std::string> args = {
            "--api", api,        "--w",      std::to_string(w),
            "--h",   std::to_string(h),      "--no-vsync",   "--no-fps",
            "--no-analyze",      "--bench",  "240",          "--shot", "_bench.png"};
        const EnvOverrides env = {{"GLOG_minloglevel", "2"}};
        const auto pr = run_process_capture(exe, args, env);
        if (!pr.spawned) {
            std::fprintf(stderr, "[错误] 启动失败：%s\n", pr.spawn_error.c_str());
            return 2;
        }
        const std::string out = strip_glog_stamps(pr.stdout_text);

        std::string cpu, pha, gpu;
        size_t pos = 0;
        while (pos < out.size()) {
            size_t eol = out.find('\n', pos);
            if (eol == std::string::npos) eol = out.size();
            const std::string line = out.substr(pos, eol - pos);
            if (cpu.empty() && line.find("帧耗时") != std::string::npos) cpu = line;
            if (pha.empty() && line.find("分相平均") != std::string::npos) pha = line;
            if (gpu.empty() && line.find("gpu] 合计") != std::string::npos) gpu = line;
            pos = eol + 1;
        }

        std::printf("RESULT dir=%s res=%dx%d run=%d avg=%sms fps=%s p50=%sms p99=%sms | "
                    "gpu_total=%sms world=%sms RenderWorld=%sms EndFrame=%sms\n",
                    build_dir.string().c_str(), w, h, r,
                    first_num(cpu, "平均").c_str(), fps_num(cpu).c_str(),
                    first_num(cpu, "中位").c_str(), first_num(cpu, "p99").c_str(),
                    first_num(gpu, "合计").c_str(), first_num(pha, "world update").c_str(),
                    first_num(pha, "RenderWorld").c_str(),
                    first_num(pha, "EndFrame(提交+等GPU)").c_str());
    }
    return 0;
}

} // namespace grycegc
