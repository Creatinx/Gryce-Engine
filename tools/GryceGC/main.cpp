// GryceGC - GryceEngine Global Compiler (GryceSPC packaging tool).
//
// 仅负责 CLI 参数解析与三种模式（init / pak / project 打包）的调度。
// 具体实现分散到各功能模块：
//   string_util / random_util / crypto      —— 字符串、随机、哈希工具
//   file_collect                            —— 资源与 core shader 收集
//   runtime_util / runtime_copy             —— 路径/DLL 定位与运行时拷贝
//   pack                                    —— .gpkg 打包与 project.data 生成
//   project_skeleton                        —— GryceGC-A 项目骨架创建
//
// Usage:
//   GryceGC --init <project-dir> [--name <name>]      create a standard GryceGC-A project
//   GryceGC --project examples/3dtest --name MyGame
//           --build-dir build --config Release --out build/game

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unordered_set>
#include <vector>

#include "GryceCore/core_api.h"
#include "resources/pak_bundle.h"
#include "resources/resource_loader.h"
#include "script/runtime/script_vm.h"

#include "file_collect.h"
#include "pack.h"
#include "project_skeleton.h"
#include "random_util.h"
#include "runtime_copy.h"
#include "runtime_util.h"
#include "string_util.h"

namespace fs = std::filesystem;
using namespace gryce_engine::gc;

namespace {

void print_usage(const char* argv0) {
    std::printf(
        "GryceGC - GryceEngine Global Compiler (GryceSPC packaging tool)\n"
        "项目创建:\n"
        "  %s --init <dir> [--name <name>]  创建标准 GryceGC-A 项目骨架\n"
        "\n"
        "打包:\n"
        "  %s --project <dir> [options]\n"
        "  --project <dir>   game project directory (res:// root) [required]\n"
        "  --name <name>     output game name (default: MyGame)\n"
        "  --build-dir <dir> CMake build directory (default: build)\n"
        "  --config <cfg>    Debug or Release (default: Release)\n"
        "  --out <dir>       output parent directory (default: build/game)\n"
        "  --game <exe>      GryceGame template exe (default: compiled-in path)\n"
        "  --author <name>   author stored in project.data (default: %%USERNAME%%)\n"
        "\n"
        "Pak mode（UI 资源发布包）:\n"
        "  --pak             enable pak mode (no project/runtime assembly)\n"
        "  --assets <dir>    assets directory to pack [required with --pak]\n"
        "  --output <file>   output .pak path (default: game.pak)\n"
        "  --dev             dev mode: pack raw files, no bytecode/encryption\n",
        argv0, argv0);
}

// CLI 解析结果。
struct Options {
    std::string project, name = "MyGame", build_dir = "build", config = "Release",
                out = "build/game", author;
    std::string init_dir, window_title;
    std::string game_exe;
    std::string assets, output_pak = "game.pak";
    bool pak_mode = false;
    bool dev_mode = false;
};

// ---- Init 模式：创建标准 GryceGC-A 项目骨架 ----
int run_init(const Options& opt) {
    // 项目名默认取目录名（转 PascalCase）；显式 --name 保持原样。
    std::string dir_name = fs::path(opt.init_dir).empty()
        ? opt.init_dir : fs::path(opt.init_dir).filename().generic_string();
    std::string name = opt.name;
    if (name == "MyGame" && !dir_name.empty() && !opt.init_dir.ends_with("\\") &&
        !opt.init_dir.ends_with("/")) {
        name = to_pascal_case(dir_name); // 目录名缺省即项目名（GryceGC-A 约定）
    }
    return create_project_skeleton(fs::path(opt.init_dir), name, opt.window_title) ? 0 : 1;
}

// ---- Pak 模式：将 assets 目录打包为发布 .pak（JS 字节码 + DSL 加密） ----
int run_pak(const Options& opt) {
    const std::vector<FileEntry> files = collect_project_files(opt.assets);
    if (files.empty()) {
        std::cerr << "[grycegc] ERROR: no packable resources found in " << opt.assets << "\n";
        return 1;
    }

    // Release 模式需要 QuickJS 编译 JS 字节码。
    GryceEngineUtils::script::ScriptVM vm;
    if (!opt.dev_mode && !vm.init()) {
        std::cerr << "[grycegc] ERROR: failed to init ScriptVM (QuickJS)\n";
        return 1;
    }

    gryce_engine::resources::PakWriter writer;
    for (const FileEntry& file : files) {
        const std::string ext = to_lower(fs::path(file.internal_path).extension().string());
        std::ifstream in(file.source_path, std::ios::binary);
        if (!in) {
            std::cerr << "[grycegc] ERROR: failed to read " << file.source_path << "\n";
            return 1;
        }
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());

        bool ok = false;
        if (!opt.dev_mode && ext == ".js") {
            const std::string text(data.begin(), data.end());
            std::string compile_err;
            std::vector<uint8_t> bc = vm.compile_script(text, file.internal_path, true, &compile_err);
            if (bc.empty()) {
                std::cerr << "[grycegc] ERROR: failed to compile " << file.internal_path
                          << ": " << compile_err << "\n";
                return 1;
            }
            ok = writer.add_buffer(file.internal_path,
                                   gryce_engine::resources::ResourceLoader::pack_js_bytecode(bc));
        } else if (!opt.dev_mode && ext == ".uif") {
            const std::string text(data.begin(), data.end());
            ok = writer.add_buffer(file.internal_path,
                                   gryce_engine::resources::ResourceLoader::pack_ui_text(text));
        } else {
            ok = writer.add_buffer(file.internal_path, data);
        }
        if (!ok) {
            std::cerr << "[grycegc] ERROR: PakWriter::add_buffer('" << file.internal_path << "') failed\n";
            return 1;
        }
    }

    if (!writer.write(opt.output_pak)) {
        std::cerr << "[grycegc] ERROR: PakWriter::write('" << opt.output_pak << "') failed\n";
        return 1;
    }

    // 读回验证。
    gryce_engine::resources::PakReader reader;
    if (!reader.open(opt.output_pak) || reader.manifest().size() != files.size()) {
        std::cerr << "[grycegc] ERROR: .pak verification failed for " << opt.output_pak << "\n";
        return 1;
    }
    std::printf("[grycegc] %s: %zu resources, %s\n", opt.output_pak.c_str(), files.size(),
                opt.dev_mode ? "dev mode (no encryption)" : "release mode (AES-256-GCM)");
    return 0;
}

// ---- 打包模式：组装独立游戏目录（exe + 运行时 DLL + *.gpkg + project.data） ----
int run_project(const Options& opt) {
    bool debug = opt.config == "Debug";
    if (!debug && opt.config != "Release") {
        std::cerr << "[grycegc] ERROR: --config must be Debug or Release\n";
        return 1;
    }

    const fs::path staging = fs::path(opt.build_dir) / "bin" / opt.config;
    // 优先从 dist 布局读取（GryceEngineUtils/lib 为运行库来源，GryceGame/ 为 exe）。
    const fs::path dist_dir = staging / "dist";
    const fs::path sdk_lib = dist_dir / "GryceEngineUtils" / "lib";
    const bool has_dist = fs::is_directory(sdk_lib);

    // 运行库 DLL 源码：优先按 --build-dir/--config 推导（dist 或 flat）；无则回退按
    // GryceGC 自身位置多级同级回溯。
    const fs::path exe_src_dir(get_exe_dir());
    fs::path bin_dir = has_dist ? sdk_lib : staging;
    if (find_sibling_dll(bin_dir, "grycecore").empty()) {
        const fs::path auto_dir = find_runtime_dir_near_exe(exe_src_dir);
        if (!auto_dir.empty()) bin_dir = auto_dir;
    }
    // 按实际 DLL（GryceCored.dll 等）推断 Debug/Release，供 MSVC CRT 拷贝使用。
    if (sibling_is_debug(bin_dir)) debug = true;

    // GryceGC 依赖 GryceGame：CMake 把模板 exe 精确路径编译进工具，--game 可覆盖。
    auto file_exists = [](const fs::path& p) {
        std::error_code e;
        return fs::is_regular_file(p, e);
    };
    fs::path exe = opt.game_exe.empty() ? fs::path() : fs::path(opt.game_exe);
    if (!file_exists(exe)) {
#ifdef GRYCE_GC_GAME_TEMPLATE
        exe = fs::path(GRYCE_GC_GAME_TEMPLATE);
#endif
    }
    if (!file_exists(exe)) {
        const fs::path dist_game = dist_dir / "GryceGame" / "GryceGame.exe";
        exe = file_exists(dist_game) ? dist_game : (staging / "GryceGame.exe");
    }
    std::error_code ec;
    if (!fs::is_regular_file(exe, ec)) {
        std::cerr << "[grycegc] ERROR: " << exe
                  << " not found; pass --game, or build the GryceGame target first\n";
        return 1;
    }
    if (!fs::is_directory(opt.project, ec)) {
        std::cerr << "[grycegc] ERROR: project directory not found: " << opt.project << "\n";
        return 1;
    }

    const fs::path out_dir = fs::path(opt.out) / opt.name;
    // Fresh package: remove any previous output for this game first.
    if (fs::exists(out_dir, ec)) {
        std::printf("[grycegc] cleaning previous output: %s\n", out_dir.string().c_str());
        fs::remove_all(out_dir, ec);
        if (ec) {
            std::cerr << "[grycegc] ERROR: failed to clean " << out_dir
                      << ": " << ec.message() << " (is the game running?)\n";
            return 1;
        }
    }
    fs::create_directories(out_dir, ec);
    const fs::path assets_dir = out_dir / "assets";
    fs::create_directories(assets_dir, ec);

    // 1) Runtime: exe at the output root, core DLLs under runtime/.
    std::vector<std::string> copied;
    if (!copy_runtime(opt.build_dir, bin_dir, debug, out_dir, exe, opt.name, copied)) {
        return 1;
    }

    // 2) Content: 每个文件单独打包成一个 .gpkg（随机 Base64 名），data 区以随机 32 字节
    //    密钥做 ChaCha20 加密（密钥最终写入 project.data）。
    const std::vector<FileEntry> files = collect_project_files(opt.project);
    if (files.empty()) {
        std::cerr << "[grycegc] ERROR: no packable resources found in project\n";
        return 1;
    }

    uint8_t key_raw[32] = {};
    if (!random_bytes(key_raw, sizeof(key_raw))) {
        std::cerr << "[grycegc] ERROR: failed to generate encryption key\n";
        return 1;
    }
    const std::string enc_key_hex = bytes_to_hex(key_raw, sizeof(key_raw));
    gryce_engine::resources::set_pak_crypto_key(
        std::string(reinterpret_cast<const char*>(key_raw), sizeof(key_raw)));

    size_t total_entries = 0;
    for (const FileEntry& file : files) {
        const fs::path bundle_path = assets_dir / (random_base64_name() + ".gpkg");
        std::vector<FileEntry> one;
        one.push_back(file);
        if (!write_bundle(one, bundle_path, total_entries)) {
            return 1;
        }
    }

    // 2.5) Core 默认 shader 兜底：引擎默认 shader 集（项目未覆盖的）打进独立 .gpkg。
    size_t core_entry_count = 0;
    const fs::path core_shaders_dir = find_core_shaders_dir(get_exe_dir());
    if (core_shaders_dir.empty()) {
        std::cerr << "[grycegc] WARNING: engine default shaders not found; "
                     "games lacking project shaders won't render.\n";
    } else {
        std::unordered_set<std::string> project_paths;
        for (const FileEntry& f : files) project_paths.insert(to_lower(f.internal_path));
        const auto core_files = collect_core_shader_files(core_shaders_dir, project_paths);
        if (!core_files.empty()) {
            std::printf("[grycegc] packaging %zu core default shaders (unoverridden)\n",
                        core_files.size());
        }
        for (const FileEntry& file : core_files) {
            const fs::path bundle_path = assets_dir / (random_base64_name() + ".gpkg");
            std::vector<FileEntry> one;
            one.push_back(file);
            if (!write_bundle(one, bundle_path, total_entries)) {
                return 1;
            }
            ++core_entry_count;
        }
    }

    const size_t assets_count = files.size() + core_entry_count;
    // 3) project.data：合并源配置 + 打包元数据 + enc_key_hex。
    std::string name = opt.name;
    std::string author = opt.author;
    if (author.empty()) {
        const char* user = std::getenv("USERNAME");
        author = (user && user[0]) ? user : "Unknown";
    }
    if (!write_project_data(opt.project, out_dir, name, author, files, enc_key_hex)) {
        return 1;
    }

    std::printf("[grycegc] packaged %s -> %s\n", name.c_str(), out_dir.string().c_str());
    std::printf("[grycegc] %s.exe + runtime/%zu DLLs + assets/%zu %s + project.data\n",
                name.c_str(), copied.size(), assets_count, ".gpkg (project resources + core default shaders)");
    std::printf("[grycegc] run with: %s (project root defaults to exe dir)\n",
                (out_dir / (name + ".exe")).string().c_str());
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    init_utf8_console();
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need = [&](const char* o) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "[grycegc] ERROR: " << o << " requires a value\n";
                return nullptr;
            }
            return argv[++i];
        };
        if (arg == "--project") { if (const char* v = need("--project")) opt.project = v; }
        else if (arg == "--init" || arg == "--new") { if (const char* v = need("--init")) opt.init_dir = v; }
        else if (arg == "--window-title") { if (const char* v = need("--window-title")) opt.window_title = v; }
        else if (arg == "--name") { if (const char* v = need("--name")) opt.name = v; }
        else if (arg == "--build-dir") { if (const char* v = need("--build-dir")) opt.build_dir = v; }
        else if (arg == "--config") { if (const char* v = need("--config")) opt.config = v; }
        else if (arg == "--out") { if (const char* v = need("--out")) opt.out = v; }
        else if (arg == "--author") { if (const char* v = need("--author")) opt.author = v; }
        else if (arg == "--game") { if (const char* v = need("--game")) opt.game_exe = v; }
        else if (arg == "--pak") { opt.pak_mode = true; }
        else if (arg == "--assets") { if (const char* v = need("--assets")) opt.assets = v; }
        else if (arg == "--output") { if (const char* v = need("--output")) opt.output_pak = v; }
        else if (arg == "--dev") { opt.dev_mode = true; }
        else if (arg == "--help" || arg == "-h") { print_usage(argv[0]); return 0; }
        else {
            std::cerr << "[grycegc] ERROR: unknown option " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    // Init 模式：创建标准 GryceGC-A 项目骨架。
    if (!opt.init_dir.empty()) {
        if (opt.pak_mode || !opt.project.empty()) {
            std::cerr << "[grycegc] ERROR: --init cannot be combined with --project/--pak\n";
            return 1;
        }
        return run_init(opt);
    }

    // Pak 模式：将 assets 目录打包为发布 .pak。
    if (opt.pak_mode) {
        if (opt.assets.empty()) {
            std::cerr << "[grycegc] ERROR: --pak requires --assets <dir>\n";
            return 1;
        }
        std::error_code ec;
        if (!fs::is_directory(opt.assets, ec)) {
            std::cerr << "[grycegc] ERROR: assets directory not found: " << opt.assets << "\n";
            return 1;
        }
        return run_pak(opt);
    }

    if (opt.project.empty()) {
        std::cerr << "[grycegc] ERROR: --project is required\n";
        print_usage(argv[0]);
        return 1;
    }
    return run_project(opt);
}