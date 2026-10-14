#include "dep_resolver.h"

#include <algorithm>
#include <cstdlib>
#include <set>
#include <string>
#include <unordered_set>

#include "pe_image.h"

namespace grycegc {

namespace {

// 引擎三 DLL 名字（lower）→ core
const std::set<std::string>& engine_core_names() {
    static const std::set<std::string> s = {
        "grycecore", "grycerenderer", "gryceplatform", "gryceaudio"
    };
    return s;
}

} // namespace

DepCategory classify(const std::string& name_lower) {
    const auto base = name_lower.substr(0, name_lower.size() - 4);  // strip .dll
    if (engine_core_names().count(base)) return DepCategory::Core;
    return DepCategory::Dependency;
}

bool is_system_dll(const std::string& name_lower) {
    static const std::set<std::string> s = {
        "kernel32.dll", "msvcrt.dll", "ntdll.dll", "user32.dll", "gdi32.dll",
        "shell32.dll", "advapi32.dll", "ole32.dll", "oleaut32.dll", "uuid.dll",
        "winmm.dll", "bcrypt.dll", "crypt32.dll", "rpcrt4.dll", "ws2_32.dll",
        "opengl32.dll", "gdiplus.dll", "shcore.dll", "dwmapi.dll", "comctl32.dll",
        "comdlg32.dll", "msimg32.dll", "version.dll", "usp10.dll", "imm32.dll",
        "setupapi.dll", "cfgmgr32.dll", "devobj.dll", "iphlpapi.dll", "powrprof.dll",
        "propsys.dll", "shellfolder.dll", "vulkan-1.dll",
        "msvcp140.dll", "vcruntime140.dll"
    };
    if (s.count(name_lower)) return true;
    if (name_lower.rfind("api-ms-win-", 0) == 0) return true;
    if (name_lower.rfind("ext-ms-win-", 0) == 0) return true;
    return false;
}

std::vector<std::filesystem::path> discover_mingw_bins() {
    std::vector<std::filesystem::path> out;
    // 1) 环境变量
    for (const char* k : {"MSYS2_ROOT", "MINGW_PREFIX", "MINGW_DIR"}) {
        if (const char* v = std::getenv(k)) {
            std::error_code ec;
            auto p = std::filesystem::path(v) / "bin";
            if (std::filesystem::is_directory(p, ec)) out.push_back(p);
        }
    }
    // 2) 常见前缀
    for (const char* root : {
             "D:/msys64/mingw64/bin", "D:/msys64/ucrt64/bin",
             "D:/msys64/clang64/bin", "C:/msys64/mingw64/bin",
             "C:/msys64/ucrt64/bin", "C:/msys64/clang64/bin"}) {
        std::error_code ec;
        if (std::filesystem::is_directory(root, ec)) {
            // 验证确实含 MinGW 运行时
            if (std::filesystem::exists(std::filesystem::path(root) / "libstdc++-6.dll", ec)) {
                out.emplace_back(root);
            }
        }
    }
    // 3) CLion 自带 MinGW
#if defined(_WIN32)
    for (const char* pat : {
             "D:/JetBrains/CLion*/bin/mingw/bin",
             "C:/Program Files/JetBrains/CLion*/bin/mingw/bin"}) {
        // 简单 glob：用通配符扫描父目录
        // 简化：扫描 JetBrains 目录
        const std::string s = pat;
        const size_t star = s.find('*');
        if (star == std::string::npos) continue;
        const std::string prefix = s.substr(0, star);
        const std::string suffix = s.substr(star + 1);
        const size_t slash1 = prefix.rfind('/');
        const size_t slash2 = suffix.rfind('/');
        const std::string base = prefix.substr(0, slash1);
        std::error_code ec;
        if (!std::filesystem::is_directory(base, ec)) continue;
        for (auto& e : std::filesystem::directory_iterator(base, ec)) {
            if (!e.is_directory(ec)) continue;
            const auto p = e.path() / "bin" / "mingw" / "bin";
            if (std::filesystem::is_directory(p, ec)) out.push_back(p);
        }
    }
#endif
    return out;
}

DepResolver::DepResolver(std::filesystem::path build_dir,
                         std::vector<std::filesystem::path> extra_dirs)
    : build_dir_(std::move(build_dir)), extra_dirs_(std::move(extra_dirs)) {}

void DepResolver::include_path_env(bool yes) { include_path_ = yes; }

void DepResolver::build_name_map() const {
    if (name_map_ready_) return;
    name_map_.clear();

    auto add_dlls_in = [&](const std::filesystem::path& d, bool recurse) {
        std::error_code ec;
        if (!std::filesystem::is_directory(d, ec)) return;
        if (recurse) {
            for (auto it = std::filesystem::recursive_directory_iterator(
                     d, std::filesystem::directory_options::skip_permission_denied, ec);
                 it != std::filesystem::recursive_directory_iterator();
                 it.increment(ec)) {
                if (ec) { ec.clear(); continue; }
                if (!it->is_regular_file(ec)) continue;
                const auto fn = it->path().filename().string();
                if (fn.size() > 4 &&
                    (fn.substr(fn.size() - 4) == ".dll" || fn.substr(fn.size() - 4) == ".DLL")) {
                    auto k = fn;
                    std::transform(k.begin(), k.end(), k.begin(), ::tolower);
                    name_map_.emplace(k, it->path());
                }
            }
        } else {
            for (auto& e : std::filesystem::directory_iterator(d, ec)) {
                if (ec) continue;
                if (!e.is_regular_file(ec)) continue;
                const auto fn = e.path().filename().string();
                if (fn.size() > 4 &&
                    (fn.substr(fn.size() - 4) == ".dll" || fn.substr(fn.size() - 4) == ".DLL")) {
                    auto k = fn;
                    std::transform(k.begin(), k.end(), k.begin(), ::tolower);
                    name_map_.emplace(k, e.path());
                }
            }
        }
    };

    if (!build_dir_.empty()) add_dlls_in(build_dir_, true);
    for (const auto& d : extra_dirs_) add_dlls_in(d, true);

    if (include_path_) {
#if defined(_WIN32)
        const char* path = std::getenv("PATH");
        if (path) {
            std::string p(path);
            size_t i = 0;
            while (i <= p.size()) {
                size_t j = p.find(';', i);
                if (j == std::string::npos) j = p.size();
                add_dlls_in(std::filesystem::path(p.substr(i, j - i)), false);
                i = j + 1;
            }
        }
#endif
    }
    name_map_ready_ = true;
}

DepResolveResult DepResolver::resolve(const std::filesystem::path& entry) const {
    build_name_map();
    DepResolveResult out;
    std::unordered_set<std::string> seen_bins;  // 已遍历二进制
    std::unordered_set<std::string> seen_names; // 已加入闭包的名字
    std::vector<std::filesystem::path> queue{entry};

    while (!queue.empty()) {
        auto bin = queue.back();
        queue.pop_back();
        std::error_code ec;
        if (!std::filesystem::is_regular_file(bin, ec)) continue;
        const std::string bin_key = bin.lexically_normal().string();
        if (seen_bins.count(bin_key)) continue;
        seen_bins.insert(bin_key);

        auto names = pe::import_dll_names(bin);
        for (const auto& nm : names) {
            std::string low = nm;
            std::transform(low.begin(), low.end(), low.begin(), ::tolower);
            if (is_system_dll(low)) continue;
            if (seen_names.count(low)) continue;
            seen_names.insert(low);
            auto it = name_map_.find(low);
            if (it != name_map_.end() && std::filesystem::is_regular_file(it->second, ec)) {
                DepNode node;
                node.name_lower = low;
                node.name_orig = nm;
                node.source = it->second;
                out.found.push_back(node);
                queue.push_back(it->second);
            } else {
                out.missing.push_back(low);
            }
        }
    }
    return out;
}

} // namespace grycegc
