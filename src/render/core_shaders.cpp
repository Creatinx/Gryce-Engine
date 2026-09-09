#include "core_shaders.h"

#include <filesystem>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace gryce_engine::render::core_shaders {

namespace {

// 当前可执行文件所在目录（编辑器中为 GryceEditor.exe，运行时为 GryceGame.exe）。
// 核心默认 shader 随可执行/DLL 部署时，会放在该目录或其近邻的子目录。
std::string exe_base_dir() {
#if defined(_WIN32)
    wchar_t buffer[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, buffer, MAX_PATH) > 0) {
        std::filesystem::path p(buffer);
        return p.parent_path().string();
    }
#else
    std::error_code ec;
    std::filesystem::path p = std::filesystem::canonical("/proc/self/exe", ec);
    if (!ec) return p.parent_path().string();
#endif
    return std::filesystem::current_path().string();
}

} // namespace

std::string engine_shaders_dir() {
    namespace fs = std::filesystem;
    const std::string base = exe_base_dir();

    // 1) 随包部署布局：可执行/DLL 目录下的 shaders/ 及其近邻副本。
    std::vector<fs::path> candidates;
    for (const char* rel : {"shaders",
                            "runtime/shaders",
                            "assets/shaders",
                            "../shaders",
                            "../assets/shaders"}) {
        candidates.push_back(fs::path(base) / rel);
    }

    // 2) 引擎源码树（开发机直接运行构建产物时）：向上定位仓库根 src/render/shaders。
    fs::path d = fs::path(base);
    for (int i = 0; i < 12 && !d.empty(); ++i) {
        if (fs::is_directory(d / "src") && fs::exists(d / "CMakeLists.txt")) {
            candidates.push_back(d / "src" / "render" / "shaders");
            break;
        }
        d = d.parent_path();
    }

    for (const fs::path& c : candidates) {
        std::error_code ec;
        // 以存在 forward_clustered 子目录作为有效标识，避免误命中无关 shaders/ 目录。
        if (fs::is_directory(c, ec) && fs::is_directory(c / "forward_clustered", ec)) {
            return c.string();
        }
    }
    return "";
}

} // namespace gryce_engine::render::core_shaders