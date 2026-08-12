// GryceGC - GryceEngine Global Compiler (GryceSPC packaging tool).
//
// Packages a game project's res:// content into one or more .gpkg resource
// archives (GPAK format) and assembles a standalone game directory:
//   * the template executable (GryceGame),
//   * the core runtime DLLs,
//   * the game content as *.gpkg archives (no raw res/ directory copy).
//
// The archives are written by GryceCore's GPackWriter through its C API
// (GCore_PackCreate / GCore_PackAddFile / GCore_PackWrite), so the on-disk
// layout always stays in sync with the reader used by the runtime
// (GPackReader; GCore_Init mounts every *.gpkg/*.gpack in the project root).
//
// Usage:
//   grycegc --project examples/3dtest --name MyGame
//           --build-dir build --config Release --out build/game
//
// The packaged game is run with:
//   build/game/MyGame/MyGame.exe            (project root defaults to exe dir)

#include "GryceCore/core_api.h"
#include "resources/gpack_bundle.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct FileEntry {
    std::string internal_path;  // forward slashes, relative to project root
    fs::path source_path;
};

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Resource categories: each category becomes its own .gpkg (multiple archives
// are supported by design). `misc` catches everything else.
const std::unordered_map<std::string, std::vector<std::string>>& category_extensions() {
    static const std::unordered_map<std::string, std::vector<std::string>> kExts = {
        {"scenes",  {".gesc", ".scene", ".tscn"}},
        {"scripts", {".lua"}},
        {"shaders", {".vert", ".frag", ".geom", ".tesc", ".tese", ".comp",
                     ".glsl", ".hlsl", ".spv"}},
        {"models",  {".obj", ".fbx", ".gltf", ".glb", ".dae", ".ply", ".stl",
                     ".3ds", ".blend"}},
        {"textures", {".png", ".jpg", ".jpeg", ".bmp", ".tga", ".dds", ".ktx",
                      ".hdr", ".exr", ".gif", ".webp"}},
        {"audio",   {".wav", ".ogg", ".mp3", ".flac", ".aac"}},
        {"fonts",   {".ttf", ".otf", ".fnt", ".woff", ".woff2"}},
        {"config",  {".json", ".gryce", ".cfg", ".ini", ".toml", ".yaml",
                     ".yml", ".mat", ".txt"}},
    };
    return kExts;
}

std::string classify(const std::string& ext) {
    for (const auto& [category, exts] : category_extensions()) {
        if (std::find(exts.begin(), exts.end(), ext) != exts.end()) {
            return category;
        }
    }
    return "misc";
}

// Files / directories that never belong in a resource archive.
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

bool copy_file_if_exists(const fs::path& src_dir, const std::string& name,
                         const fs::path& dst_dir, std::vector<std::string>& copied) {
    std::error_code ec;
    const fs::path src = src_dir / name;
    if (!fs::is_regular_file(src, ec)) return false;
    fs::copy_file(src, dst_dir / name, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        std::cerr << "[grycegc] ERROR: failed to copy " << src << ": " << ec.message() << "\n";
        return false;
    }
    copied.push_back(name);
    return true;
}

bool copy_runtime(const fs::path& bin_dir, bool debug, const fs::path& out_dir,
                  const fs::path& exe, const std::string& name,
                  std::vector<std::string>& copied) {
    const std::string suffix = debug ? "d" : "";
    const std::vector<std::string> cores = {
        "GryceCore" + suffix + ".dll",
        "GryceRenderer" + suffix + ".dll",
        "GrycePlatform" + suffix + ".dll",
        "GrycePhysics" + suffix + ".dll",
    };
    for (const std::string& dll : cores) {
        std::error_code ec;
        fs::path src = bin_dir / dll;
        if (!fs::is_regular_file(src, ec)) {
            // MinGW builds name the DLL libGryceCore(d).dll
            src = bin_dir / ("lib" + dll);
            if (!fs::is_regular_file(src, ec)) {
                std::cerr << "[grycegc] warning: " << dll << " not found in " << bin_dir << "\n";
                continue;
            }
        }
        const std::string dst_name = src.filename().string();
        fs::copy_file(src, out_dir / dst_name, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            std::cerr << "[grycegc] ERROR: failed to copy " << src << ": " << ec.message() << "\n";
            return false;
        }
        copied.push_back(dst_name);
    }
    // GLFW: MSVC Debug builds use glfw3d.dll; MinGW uses plain glfw3.dll.
    bool glfw_ok = false;
    for (const char* glfw : {"glfw3d.dll", "glfw3.dll"}) {
        if (copy_file_if_exists(bin_dir, glfw, out_dir, copied)) {
            glfw_ok = true;
            break;
        }
    }
    if (!glfw_ok) {
        std::cerr << "[grycegc] warning: glfw3d.dll/glfw3.dll not found in " << bin_dir << "\n";
    }
    // MinGW runtime DLLs (self-contained packages; harmless to include).
    for (const char* rt : {"libgcc_s_seh-1.dll", "libstdc++-6.dll", "libwinpthread-1.dll"}) {
        copy_file_if_exists(bin_dir, rt, out_dir, copied);
    }

    std::error_code ec;
    fs::copy_file(exe, out_dir / (name + ".exe"), fs::copy_options::overwrite_existing, ec);
    if (ec) {
        std::cerr << "[grycegc] ERROR: failed to copy " << exe << ": " << ec.message() << "\n";
        return false;
    }
    return true;
}

bool write_bundle(const std::vector<FileEntry>& files, const fs::path& output_path,
                  size_t& entry_count) {
    GPackHandle handle = GCore_PackCreate();
    if (!handle) {
        std::cerr << "[grycegc] ERROR: GCore_PackCreate failed for " << output_path << "\n";
        return false;
    }
    bool ok = true;
    for (const FileEntry& file : files) {
        const std::string src = file.source_path.string();
        if (GCore_PackAddFile(handle, file.internal_path.c_str(), src.c_str()) != 0) {
            std::cerr << "[grycegc] ERROR: GCore_PackAddFile('" << file.internal_path << "') failed\n";
            ok = false;
            break;
        }
    }
    if (ok && GCore_PackWrite(handle, output_path.string().c_str()) != 0) {
        std::cerr << "[grycegc] ERROR: GCore_PackWrite('" << output_path << "') failed\n";
        ok = false;
    }
    GCore_PackDestroy(handle);
    if (!ok) return false;

    // Read-back verification using the same reader the runtime uses.
    gryce_engine::resources::GPackReader reader;
    if (!reader.open(output_path.string())) {
        std::cerr << "[grycegc] ERROR: verification failed to open " << output_path << "\n";
        return false;
    }
    if (reader.entries().size() != files.size()) {
        std::cerr << "[grycegc] ERROR: verification mismatch in " << output_path << "\n";
        return false;
    }
    entry_count += reader.entries().size();
    const uintmax_t size = fs::file_size(output_path);
    std::printf("[grycegc] %s: %zu files, %.2f MiB\n",
                output_path.filename().string().c_str(), entry_count,
                static_cast<double>(size) / (1024.0 * 1024.0));
    return true;
}

void print_usage(const char* argv0) {
    std::printf(
        "GryceGC - GryceEngine Global Compiler (GryceSPC packaging tool)\n"
        "Usage: %s --project <dir> [options]\n"
        "  --project <dir>   game project directory (res:// root) [required]\n"
        "  --name <name>     output game name (default: MyGame)\n"
        "  --build-dir <dir> CMake build directory (default: build)\n"
        "  --config <cfg>    Debug or Release (default: Release)\n"
        "  --out <dir>       output parent directory (default: build/game)\n"
        "  --single          pack everything into one <name>.gpkg\n",
        argv0);
}

} // namespace

int main(int argc, char* argv[]) {
    std::string project, name = "MyGame", build_dir = "build", config = "Release",
                out = "build/game";
    bool single = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need = [&](const char* opt) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "[grycegc] ERROR: " << opt << " requires a value\n";
                return nullptr;
            }
            return argv[++i];
        };
        if (arg == "--project") {
            if (const char* v = need("--project")) project = v;
        } else if (arg == "--name") {
            if (const char* v = need("--name")) name = v;
        } else if (arg == "--build-dir") {
            if (const char* v = need("--build-dir")) build_dir = v;
        } else if (arg == "--config") {
            if (const char* v = need("--config")) config = v;
        } else if (arg == "--out") {
            if (const char* v = need("--out")) out = v;
        } else if (arg == "--single") {
            single = true;
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else {
            std::cerr << "[grycegc] ERROR: unknown option " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    if (project.empty()) {
        std::cerr << "[grycegc] ERROR: --project is required\n";
        print_usage(argv[0]);
        return 1;
    }
    const bool debug = config == "Debug";
    if (!debug && config != "Release") {
        std::cerr << "[grycegc] ERROR: --config must be Debug or Release\n";
        return 1;
    }

    const fs::path bin_dir = fs::path(build_dir) / "bin" / config;
    const fs::path exe = bin_dir / "GryceGame.exe";
    std::error_code ec;
    if (!fs::is_regular_file(exe, ec)) {
        std::cerr << "[grycegc] ERROR: " << exe << " not found; build the GryceGame target first\n";
        return 1;
    }
    if (!fs::is_directory(project, ec)) {
        std::cerr << "[grycegc] ERROR: project directory not found: " << project << "\n";
        return 1;
    }

    const fs::path out_dir = fs::path(out) / name;
    fs::create_directories(out_dir, ec);

    // 1) Runtime: exe + core DLLs (same layout as before, no res/ copy).
    std::vector<std::string> copied;
    if (!copy_runtime(bin_dir, debug, out_dir, exe, name, copied)) {
        return 1;
    }

    // 2) Content: group project files into .gpkg archives.
    const std::vector<FileEntry> files = collect_project_files(project);
    if (files.empty()) {
        std::cerr << "[grycegc] ERROR: no packable resources found in project\n";
        return 1;
    }

    std::map<std::string, std::vector<FileEntry>> bundles;
    for (const FileEntry& file : files) {
        const std::string ext = to_lower(fs::path(file.internal_path).extension().string());
        const std::string key = single ? "all" : classify(ext);
        bundles[key].push_back(file);
    }

    size_t total_entries = 0;
    for (const auto& [key, members] : bundles) {
        const std::string suffix = single ? "" : "." + key;
        const fs::path bundle_path = out_dir / (name + suffix + ".gpkg");
        if (!write_bundle(members, bundle_path, total_entries)) {
            return 1;
        }
    }

    std::printf("[grycegc] packaged %s -> %s\n", name.c_str(), out_dir.string().c_str());
    std::printf("[grycegc] exe + %zu runtime DLLs + %zu resources in %zu .gpkg archive(s)\n",
                copied.size(), total_entries, bundles.size());
    std::printf("[grycegc] run with: %s (project root defaults to exe dir)\n",
                (out_dir / (name + ".exe")).string().c_str());
    return 0;
}
