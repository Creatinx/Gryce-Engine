#pragma once

// ---------------------------------------------------------------------------
// texture_path.h — 模型文件里贴图引用的路径解析（各格式导入器共用）
//
// 模型文件里写贴图路径的方式完全不统一：
//   "textures/a.png"   → 相对模型（或 MTL）所在目录
//   "D:\art\wood.jpg"  → 导出机器的绝对路径，换台机器必然失效
//   "..\maps\a.tga"    → 上面两种情况混着反斜杠
//   "Wood.JPG"         → 大小写在 Windows 上无所谓，但 macOS/Linux 导出的模型
//                         对不上 NTFS 上的实际文件名，照样 fopen 失败
// 统一走 resolve()：先按原样试，再按文件名在模型目录 / 常见贴图子目录里找
// （含大小写不敏感扫描）。找不到返回空串，调用方退回标量材质参数。
//
// 头文件形式（inline）是为了同时被 assimp 导入器与内置 OBJ 加载器使用，
// 而不必为了几个小函数再拆一个编译单元。
// ---------------------------------------------------------------------------

#include <cstddef>
#include <filesystem>
#include <string>

namespace gryce_engine::assets::texture_path {

inline std::string normalize_separators(std::string path) {
    for (char& c : path) {
        if (c == '\\') c = '/';
    }
    return path;
}

inline bool iequals_ascii(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return true;
}

inline bool looks_absolute(const std::string& path) {
    if (path.size() >= 2 && path[1] == ':') return true;                    // C:/...
    if (path.size() >= 2 && path[0] == '/' && path[1] == '/') return true;  // //server/share
    return !path.empty() && path[0] == '/';
}

// 目录内按文件名（大小写不敏感）查找
inline std::string find_file_ci(const std::filesystem::path& dir, const std::string& filename) {
    if (filename.empty()) return {};
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return {};
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        if (iequals_ascii(entry.path().filename().string(), filename)) {
            return normalize_separators(entry.path().string());
        }
    }
    return {};
}

// base_dir 需带尾部斜杠（"models/"）；raw 为模型文件里的原始字符串。
inline std::string resolve(const std::string& base_dir, const std::string& raw) {
    const std::string normalized = normalize_separators(raw);
    std::error_code ec;

    if (looks_absolute(normalized)) {
        // 绝对路径只认"真的存在"这一个条件：换机后它通常是导出机的垃圾路径，
        // 直接返回会让上层拿到一个打不开的文件名。
        if (std::filesystem::is_regular_file(std::filesystem::path(normalized), ec)) {
            return normalized;
        }
    } else {
        const std::string candidate = normalize_separators(base_dir + normalized);
        if (std::filesystem::is_regular_file(std::filesystem::path(candidate), ec)) {
            return candidate;
        }
    }

    const std::string filename = std::filesystem::path(normalized).filename().string();
    if (filename.empty()) return {};
    // 模型与贴图分开打包时最常见的几种布局
    static const char* k_sub_dirs[] = {"",          "textures/", "Textures/", "maps/",
                                       "Maps/",     "texture/",  "../textures/", "../maps/"};
    for (const char* sub : k_sub_dirs) {
        const std::string dir = normalize_separators(base_dir + sub);
        const std::string candidate = normalize_separators(dir + filename);
        if (std::filesystem::is_regular_file(std::filesystem::path(candidate), ec)) {
            return candidate;
        }
        const std::string hit = find_file_ci(std::filesystem::path(dir), filename);
        if (!hit.empty()) return hit;
    }
    return {};
}

} // namespace gryce_engine::assets::texture_path
