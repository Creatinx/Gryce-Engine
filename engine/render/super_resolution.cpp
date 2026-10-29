// super_resolution.cpp — 超分与 DLSS 运行时探测实现
//
// 只做文件/符号层面的探测：找到 sl.interposer.dll 就解析 slInit 与
// slSetFeatureLoaded 两个导出，再检查 nvngx_dlss.dll 是否在搜索路径内。
// 不调用 slInit、不碰设备创建 —— 探测结果用于如实回报可用性，并让上层决定
// 降级到内置时域重建还是继续用 DLSS。
#include "render/super_resolution.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "utils/glog/glog_lib.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace gryce_engine::render {

namespace {

// 进程内缓存：探测只在第一次调用时执行。
SuperResolutionCaps g_caps;
bool g_probed = false;

std::string lower_copy(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool file_exists(const std::string& path) {
#if defined(_WIN32)
    DWORD attr = GetFileAttributesA(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
    (void)path;
    return false;
#endif
}

std::string exe_dir() {
#if defined(_WIN32)
    char buf[MAX_PATH]{};
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::string();
    std::string path(buf, n);
    size_t pos = path.find_last_of("\\/");
    return pos == std::string::npos ? std::string() : path.substr(0, pos);
#else
    return std::string();
#endif
}

std::string join_path(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    char last = dir.back();
    if (last == '\\' || last == '/') return dir + name;
    return dir + "/" + name;
}

// 在候选目录里找第一个存在的文件，返回完整路径（找不到返回空）。
std::string find_file(const std::vector<std::string>& dirs, const std::string& name) {
    for (const auto& d : dirs) {
        if (d.empty()) continue;
        std::string full = join_path(d, name);
        if (file_exists(full)) return full;
    }
    return std::string();
}

#if defined(_WIN32)
// 解析一个导出符号是否存在；不保留句柄（进程生命周期内 DLL 不会被卸载）。
bool has_export(HMODULE module, const char* symbol) {
    return module != nullptr && GetProcAddress(module, symbol) != nullptr;
}
#endif

const char* const k_interposer = "sl.interposer.dll";
const char* const k_dlss_plugin = "sl.dlss.dll";
const char* const k_ngx_dlss = "nvngx_dlss.dll";

void probe_impl(const std::vector<std::string>& extra_dirs) {
    std::vector<std::string> dirs;
    dirs.push_back(exe_dir());
    for (const auto& d : extra_dirs) dirs.push_back(d);
    // 源码树里的 SDK 位置（开发者本机放好 bin/x64 后可直接命中）
    const std::string exe = exe_dir();
    if (!exe.empty()) {
        dirs.push_back(join_path(exe, ".."));
        dirs.push_back(join_path(exe, "../.."));
    }
    dirs.push_back("third_party/streamline/bin/x64");
    dirs.push_back("./");

    SuperResolutionCaps caps;

    const std::string interposer = find_file(dirs, k_interposer);
    caps.interposer_path = interposer;
    const std::string dlss_plugin = find_file(dirs, k_dlss_plugin);
    const std::string ngx = find_file(dirs, k_ngx_dlss);
    caps.ngx_runtime = !ngx.empty();

#if defined(_WIN32)
    if (!interposer.empty()) {
        // LOAD_WITH_ALTERED_SEARCH_PATH：插件依赖同目录下的其它 DLL。
        HMODULE sl = LoadLibraryExA(interposer.c_str(), nullptr,
                                    LOAD_WITH_ALTERED_SEARCH_PATH);
        if (sl != nullptr) {
            const bool has_init = has_export(sl, "slInit");
            const bool has_feature = has_export(sl, "slSetFeatureLoaded") &&
                                     has_export(sl, "slEvaluateFeature");
            caps.streamline_runtime = has_init && has_feature;
        }
    }
#endif

    // DLSS 的硬前置：设备（VkInstance/VkDevice）必须经由 interposer 代理创建，
    // Streamline 才能拿到内部的资源句柄。本引擎当前直连 loader 建设备，因此
    // 即使运行时齐备也只能回报「待接线」，不会让渲染路径跑到未验证分支上。
    caps.device_proxied = false;
    caps.dlss_usable = caps.streamline_runtime && caps.ngx_runtime &&
                       !dlss_plugin.empty() && caps.device_proxied;

    if (caps.dlss_usable) {
        caps.mode = SuperResolutionMode::Dlss;
        caps.detail = "DLSS 可用：Streamline 运行时与 NGX DLSS 均已就位";
    } else if (interposer.empty() && ngx.empty()) {
        caps.mode = SuperResolutionMode::Temporal;
        caps.detail =
            "DLSS 不可用：未找到 sl.interposer.dll 与 nvngx_dlss.dll。"
            "头文件已随仓库提供（third_party/streamline/include，SL 2.14.16），"
            "把 DLSS SDK 的运行时（sl.interposer.dll / sl.dlss.dll / nvngx_dlss.dll）"
            "放到可执行文件目录后重启即可被探测到；当前使用内置时域重建。";
    } else if (!caps.streamline_runtime) {
        caps.mode = SuperResolutionMode::Temporal;
        caps.detail =
            "DLSS 不可用：找到 sl.interposer.dll 但缺少 slInit / slEvaluateFeature "
            "导出（版本不匹配或文件损坏）；当前使用内置时域重建。";
    } else if (!caps.ngx_runtime) {
        caps.mode = SuperResolutionMode::Temporal;
        caps.detail =
            "DLSS 不可用：Streamline 运行时已就位，但缺少 nvngx_dlss.dll；"
            "当前使用内置时域重建。";
    } else if (dlss_plugin.empty()) {
        caps.mode = SuperResolutionMode::Temporal;
        caps.detail =
            "DLSS 不可用：Streamline 与 NGX 已就位，但缺少 sl.dlss.dll 插件；"
            "当前使用内置时域重建。";
    } else {
        caps.mode = SuperResolutionMode::Temporal;
        caps.detail =
            "DLSS 运行时齐备，但设备创建尚未经 interposer 代理（见 "
            "docs/zh/rt-reflection.md 的 DLSS 接入一节）；当前使用内置时域重建。";
    }
    g_caps = caps;
    g_probed = true;
    GLOG_INFO("SuperResolution: {} — {}", super_resolution_mode_name(g_caps.mode),
              g_caps.detail);
}

}  // namespace

const SuperResolutionCaps& probe_super_resolution(const std::vector<std::string>& extra_dirs) {
    if (!g_probed) probe_impl(extra_dirs);
    return g_caps;
}

const char* super_resolution_mode_name(SuperResolutionMode mode) {
    switch (mode) {
        case SuperResolutionMode::Off: return "off";
        case SuperResolutionMode::Dlss: return "dlss";
        case SuperResolutionMode::Temporal:
        default: return "temporal";
    }
}

SuperResolutionMode parse_super_resolution_mode(const std::string& text) {
    const std::string v = lower_copy(text);
    if (v == "off" || v == "none" || v == "native") return SuperResolutionMode::Off;
    if (v == "dlss") return SuperResolutionMode::Dlss;
    return SuperResolutionMode::Temporal;
}

} // namespace gryce_engine::render
