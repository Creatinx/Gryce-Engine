#include "file_collect.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "string_util.h"

namespace fs = std::filesystem;

namespace gryce_engine::gc {

namespace {

const std::vector<std::string>& skip_dirs() {
    static const std::vector<std::string> kDirs = {
        ".git", ".vs", ".idea", "__pycache__", "build", "bin", "obj",
        "x64", "out",
    };
    return kDirs;
}

const std::vector<std::string>& skip_extensions() {
    static const std::vector<std::string> kExts = {
        ".cpp", ".cc", ".cxx", ".c", ".h", ".hpp", ".hh", ".inl",
        ".py", ".md", ".sln", ".pdb", ".ilk", ".exp", ".lib", ".dll",
        ".exe",
    };
    return kExts;
}

const std::vector<std::string>& skip_names() {
    static const std::vector<std::string> kNames = {
        "cmakelists.txt", ".gitignore", ".gitattributes", "license", "readme.md",
    };
    return kNames;
}

bool contains_ci(const std::vector<std::string>& items, const std::string& value) {
    return std::find(items.begin(), items.end(), value) != items.end();
}

} // namespace

std::string read_file_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::vector<FileEntry> collect_project_files(const fs::path& project_root) {
    std::vector<FileEntry> files;
    std::error_code ec;
    fs::recursive_directory_iterator it(project_root, ec);
    const fs::recursive_directory_iterator end;
    for (; it != end && !ec; it.increment(ec)) {
        const fs::directory_entry& entry = *it;
        const std::string name = to_lower(entry.path().filename().string());
        if (entry.is_directory(ec)) {
            if (contains_ci(skip_dirs(), name)) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (!entry.is_regular_file(ec)) continue;
        if (contains_ci(skip_names(), name)) continue;
        const std::string ext = to_lower(entry.path().extension().string());
        if (contains_ci(skip_extensions(), ext)) continue;

        fs::path rel = fs::relative(entry.path(), project_root, ec);
        if (ec) continue;
        std::string internal = rel.generic_string();
        while (!internal.empty() && internal.front() == '/') internal.erase(0, 1);
        while (internal.rfind("./", 0) == 0) internal.erase(0, 2);
        if (internal.empty()) continue;
        files.push_back({std::move(internal), entry.path()});
    }
    std::sort(files.begin(), files.end(),
              [](const FileEntry& a, const FileEntry& b) { return a.internal_path < b.internal_path; });
    return files;
}

std::vector<FileEntry> collect_core_shader_files(
    const fs::path& engine_shaders,
    const std::unordered_set<std::string>& project_paths) {
    std::vector<FileEntry> files;
    std::error_code ec;
    fs::recursive_directory_iterator it(engine_shaders, ec);
    const fs::recursive_directory_iterator end;
    for (; it != end && !ec; it.increment(ec)) {
        const fs::directory_entry& entry = *it;
        if (entry.is_directory(ec)) continue;
        if (!entry.is_regular_file(ec)) continue;
        const std::string ext = to_lower(entry.path().extension().string());
        if (ext != ".vert" && ext != ".frag") continue;
        fs::path rel = fs::relative(entry.path(), engine_shaders, ec);
        if (ec) continue;
        const std::string internal = to_lower(std::string("shaders/") + rel.generic_string());
        if (internal == "shaders/") continue;
        if (project_paths.count(internal)) continue; // 项目覆盖优先
        files.push_back({internal, entry.path()});
    }
    std::sort(files.begin(), files.end(),
              [](const FileEntry& a, const FileEntry& b) { return a.internal_path < b.internal_path; });
    return files;
}

} // namespace gryce_engine::gc