#include "runtime_util.h"

#include <cstdint>
#include <cstdlib>
#include <fstream>

#include "string_util.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <climits>
#else
#include <unistd.h>
#include <climits>
#endif

namespace fs = std::filesystem;

namespace gryce_engine::gc {

void init_utf8_console() {
#if defined(_WIN32)
    if (SetConsoleOutputCP(CP_UTF8)) SetConsoleCP(CP_UTF8);
#endif
}

std::string get_exe_dir() {
#if defined(_WIN32)
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    const fs::path p(buf);
    return p.has_parent_path() ? p.parent_path().generic_string() : std::string();
#elif defined(__linux__)
    char buf[4096];
    const auto n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    buf[n] = '\0';
    const fs::path p(buf);
    return p.has_parent_path() ? p.parent_path().generic_string() : std::string();
#else
    return {};
#endif
}

fs::path find_sibling_dll(const fs::path& dir, const std::string& prefix) {
    std::error_code ec;
    fs::directory_iterator it(dir, ec);
    for (const auto& entry : it) {
        std::error_code e2;
        if (!entry.is_regular_file(e2)) continue;
        const std::string name = to_lower(entry.path().filename().string());
        if (name.size() < 5 || name.compare(name.size() - 4, 4, ".dll") != 0) continue;
        if (name.compare(0, prefix.size(), prefix) == 0) return entry.path();
    }
    return {};
}

fs::path find_shaderc_dll() {
#if defined(_WIN32)
    std::vector<fs::path> cands;
    const char* env = std::getenv("VULKAN_SDK");
    if (!env || !*env) env = std::getenv("VULKAN_SDK_DIR");
    if (env && *env) cands.push_back(fs::path(env) / "Bin" / "shaderc_shared.dll");
    cands.push_back(fs::path("C:/VulkanSDK") / "Bin" / "shaderc_shared.dll");
    for (const fs::path& c : cands) {
        std::error_code e;
        if (fs::is_regular_file(c, e)) return c;
    }
#else
    // POSIX: runtime loader probes libshaderc_shared.so / libshaderc_combined.so
    // from the process loader path; nothing to stage beside the exe here.
#endif
    return {};
}

bool sibling_is_debug(const fs::path& dir) {
    const fs::path core = find_sibling_dll(dir, "grycecore");
    if (core.empty()) return false;
    const std::string stem = core.stem().string();
    return !stem.empty() && (stem.back() == 'd' || stem.back() == 'D');
}

fs::path find_core_shaders_dir(const fs::path& exe_dir) {
    std::error_code ec;
    for (const char* rel : {"shaders", "runtime/shaders", "assets/shaders", "../shaders"}) {
        const fs::path c = exe_dir / rel;
        if (fs::is_directory(c / "forward_clustered", ec)) return c;
        ec.clear();
    }
    fs::path d = exe_dir;
    for (int i = 0; i < 12 && !d.empty(); ++i) {
        if (fs::is_directory(d / "src", ec) && fs::exists(d / "CMakeLists.txt", ec)) {
            const fs::path c = d / "src" / "render" / "shaders";
            if (fs::is_directory(c / "forward_clustered", ec)) return c;
            return fs::path();
        }
        d = d.parent_path();
    }
    return fs::path();
}

std::vector<fs::path> runtime_candidates(const fs::path& exe_dir) {
    std::vector<fs::path> out;
    out.push_back(exe_dir);
    out.push_back(exe_dir / "runtime");
    fs::path up = exe_dir;
    for (int i = 0; i < 3; ++i) {
        if (up.has_parent_path()) up = up.parent_path(); else break;
        out.push_back(up / "GryceEngineUtils" / "lib");
        out.push_back(up / "runtime");
        out.push_back(up);
    }
    return out;
}

fs::path find_runtime_dir_near_exe(const fs::path& exe_dir) {
    for (const fs::path& c : runtime_candidates(exe_dir)) {
        std::error_code e;
        if (fs::is_directory(c, e) && !find_sibling_dll(c, "grycecore").empty()) return c;
    }
    return {};
}

fs::path find_msvc_crt_dir(const fs::path& build_dir, bool debug) {
    std::vector<fs::path> installs;
    std::error_code ec;

    // 1) The VS generator instance recorded in the CMake cache.
    std::ifstream cache(build_dir / "CMakeCache.txt");
    if (cache) {
        std::string line;
        while (std::getline(cache, line)) {
            const std::string key = "CMAKE_GENERATOR_INSTANCE:INTERNAL=";
            if (line.rfind(key, 0) == 0) {
                std::string v = line.substr(key.size());
                if (!v.empty()) installs.push_back(v);
                break;
            }
        }
    }

    // 2) Common VS install roots (VS2022/2026 + BuildTools).
    for (const char* root : {
             "C:\\Program Files\\Microsoft Visual Studio",
             "C:\\Program Files (x86)\\Microsoft Visual Studio",
             "D:\\Microsoft Visual Studio"}) {
        std::error_code ec2;
        for (const auto& edition : fs::directory_iterator(root, ec2)) {
            if (!edition.is_directory()) continue;
            for (const auto& inst : fs::directory_iterator(edition.path(), ec2)) {
                if (inst.is_directory()) installs.push_back(inst.path());
            }
        }
    }

    const char* want = debug ? "vcruntime140d.dll" : "vcruntime140.dll";
    fs::path best;
    uint64_t best_version = 0;
    for (const fs::path& inst : installs) {
        std::error_code ec3;
        const fs::path redist = inst / "VC" / "Redist" / "MSVC";
        for (const auto& ver : fs::directory_iterator(redist, ec3)) {
            if (!ver.is_directory()) continue;
            uint64_t vnum = 0;
            try {
                vnum = std::stoull(ver.path().filename().string());
            } catch (...) {
                continue;
            }
            if (vnum < best_version) continue;
            const fs::path arch_dir = debug
                ? ver.path() / "debug_nonredist" / "x64"
                : ver.path() / "x64";
            std::error_code ec4;
            for (const auto& pkg : fs::directory_iterator(arch_dir, ec4)) {
                if (!pkg.is_directory()) continue;
                const std::string name = pkg.path().filename().string();
                const bool is_crt = debug
                    ? name.find(".DebugCRT") != std::string::npos
                    : name.find(".CRT") != std::string::npos;
                if (!is_crt) continue;
                if (fs::is_regular_file(pkg.path() / want, ec4)) {
                    best = pkg.path();
                    best_version = vnum;
                }
            }
        }
    }
    if (!best.empty()) return best;

    // 3) Fallback: System32 (the redistributable installed for the build).
    return fs::path("C:\\Windows\\System32");
}

} // namespace gryce_engine::gc