// GryceGC - GryceEngine Global Compiler (GryceSPC packaging tool).
//
// Packages a game project's res:// content into one or more .gpkg resource
// archives (GPAK v3 with random Base64 store names) and assembles a standalone
// game directory:
//   <out>/<name>/<name>.exe         template executable
//   <out>/<name>/runtime/           core runtime DLLs
//   <out>/<name>/assets/*.gpkg      game content archives (one encrypted .gpkg
//                                   per resource; GPAK v4, random Base64 names)
//   <out>/<name>/project.data      the single config + metadata file in the game
//                                   root (JSON): project manifest + runtime
//                                   settings merged, plus the pack metadata that
//                                   used to live in gdata — source-file records,
//                                   a 64-byte SHA-512 key, the ChaCha20 decryption
//                                   key (enc_key_hex) and author info.
//
// The archives are written by GryceCore's PakWriter (GPAK v4: random Base64
// store names + a manifest mapping them back to logical paths, data area
// ChaCha20-encrypted when a 32-byte key is set), so the on-disk layout always
// stays in sync with the reader used by the runtime (PakReader; GCore_Init
// mounts every *.gpkg/*.gpack in the project root). PakReader understands GPAK
// v1, v3 and v4, so legacy archives keep working.
//
// Usage:
//   GryceGC --init <project-dir> [--name <name>]      create a standard GryceGC-A project
//   GryceGC --project examples/3dtest --name MyGame
//           --build-dir build --config Release --out build/game
//
// The packaged game is run with:
//   build/game/MyGame/MyGame.exe            (project root defaults to exe dir)

#include "GryceCore/core_api.h"
#include "resources/pak_bundle.h"
#include "resources/resource_loader.h"
#include "script/runtime/script_vm.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#endif

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

// Normalize a default project name (derived from the --init target directory)
// to PascalCase, e.g. "my-game" -> "MyGame", "ecs_demo" -> "EcsDemo". Explicit
// --name values are always used verbatim, never passed through here.
std::string to_pascal_case(std::string s) {
    std::string out;
    bool at_word_start = true;
    for (unsigned char c : s) {
        if (std::isalnum(c)) {
            if (at_word_start) out.push_back(static_cast<char>(std::toupper(c)));
            else out.push_back(static_cast<char>(std::tolower(c)));
            at_word_start = false;
        } else {
            at_word_start = true;  // non-alnum separates words
        }
    }
    if (out.empty()) out = "MyGame";
    return out;
}

// 随机字节（Windows CNG / Linux /dev/urandom）
bool random_bytes(uint8_t* out, size_t len) {
#if defined(_WIN32)
    return BCryptGenRandom(nullptr, out, static_cast<ULONG>(len),
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    FILE* f = std::fopen("/dev/urandom", "rb");
    if (!f) return false;
    const size_t n = std::fread(out, 1, len, f);
    std::fclose(f);
    return n == len;
#endif
}

std::string bytes_to_hex(const uint8_t* data, size_t len) {
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(kHex[data[i] >> 4]);
        out.push_back(kHex[data[i] & 0x0F]);
    }
    return out;
}

// 32 字节随机数 -> Base64URL 编码（43 字符，无填充），用于 .gpkg 包文件名。
std::string random_base64_name() {
    uint8_t bytes[32];
    if (!random_bytes(bytes, sizeof(bytes))) {
        static std::mt19937_64 rng(std::chrono::steady_clock::now().time_since_epoch().count());
        for (size_t i = 0; i < sizeof(bytes); i += 8) {
            const uint64_t v = rng();
            std::memcpy(bytes + i, &v, sizeof(bytes) - i < 8 ? sizeof(bytes) - i : 8);
        }
    }
    static const char kBase64Url[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve(43);
    for (size_t i = 0; i < sizeof(bytes); i += 3) {
        const uint32_t b = (static_cast<uint32_t>(bytes[i]) << 16) |
                           (i + 1 < sizeof(bytes) ? static_cast<uint32_t>(bytes[i + 1]) << 8 : 0) |
                           (i + 2 < sizeof(bytes) ? static_cast<uint32_t>(bytes[i + 2]) : 0);
        out.push_back(kBase64Url[(b >> 18) & 0x3F]);
        out.push_back(kBase64Url[(b >> 12) & 0x3F]);
        out.push_back(i + 1 < sizeof(bytes) ? kBase64Url[(b >> 6) & 0x3F] : '=');
        out.push_back(i + 2 < sizeof(bytes) ? kBase64Url[b & 0x3F] : '=');
    }
    while (!out.empty() && out.back() == '=') out.pop_back();
    return out;
}

// The tool prints UTF-8 Chinese to the console; the default Windows console is
// GBK (CP936) and mangles it. Switch stdout/stderr to UTF-8 so log lines with
// 中文 render correctly in both cmd and Windows Terminal.
void init_utf8_console() {
#if defined(_WIN32)
    if (SetConsoleOutputCP(CP_UTF8)) SetConsoleCP(CP_UTF8);
#endif
}

// Directory that contains the running GryceGC executable. In the standard build
// layout the engine runtime DLLs sit at the same level (同级), so this is the
// default source for the DLLs copied into the packaged game.
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

// Locate an engine runtime DLL in dir whose (lower-cased) name starts with the
// given prefix, tolerating the debug `d` suffix and a `lib` prefix. E.g. prefix
// "grycecore" matches GryceCore.dll / GryceCored.dll / libGryceCore.dll, and
// "glfw3" matches glfw3.dll / glfw3d.dll. Returns empty on no match.
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

// Locate the Vulkan shader compiler runtime DLL (shaderc_shared.dll) shipped
// with a Vulkan SDK. It is loaded at runtime via LoadLibrary/dlopen (see
// vk_glsl_compiler.cpp), so it has no link-time dependency and is optional for
// GL-only runtimes. Resolution mirrors assemble_dist.py: prefer VULKAN_SDK env,
// fall back to the default install root. Returns empty on no match.
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

// Whether the sibling DLLs in dir are a debug build, inferred from the core
// runtime DLL carrying the `d` suffix (e.g. GryceCored.dll).
bool sibling_is_debug(const fs::path& dir) {
    const fs::path core = find_sibling_dll(dir, "grycecore");
    if (core.empty()) return false;
    const std::string stem = core.stem().string();
    return !stem.empty() && (stem.back() == 'd' || stem.back() == 'D');
}

// 定位引擎默认 shader 目录（core 全套源码）。优先随包部署副本（GryceGC 同级的
// shaders/runtime/shaders/assets/shaders），否则上溯到仓库根找 src/render/shaders。
// 以存在 forward_clustered 子目录作为有效标识，避免误命中无关 shaders/ 目录。
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

// 收集 core 默认 shader 源文件（internal 前缀 "shaders/"），跳过项目已覆盖的
// 同 internal 路径文件——这样打包产物里任意 shader 键只有一份，bundle 提取
// 天然确定"项目覆盖优先、core 兜底"的顺序。打的是源文件（.vert/.frag），
// 首次运行才由 GL/Vulkan 后端编译。
std::vector<FileEntry> collect_core_shader_files(const fs::path& engine_shaders,
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

// Candidate runtime DLL locations relative to the GryceGC executable (读取依赖
// dist 同级 runtime/)。Each candidate may be a flat dir (DLLs next to the exe)
// or a dist layout: engine DLLs sit in the target's own `runtime/` subfolder or
// under the sibling GryceEngineUtils/lib (a few levels up from dist/GryceGC/).
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

// Return the first candidate dir that actually contains an engine core DLL.
fs::path find_runtime_dir_near_exe(const fs::path& exe_dir) {
    for (const fs::path& c : runtime_candidates(exe_dir)) {
        std::error_code e;
        if (fs::is_directory(c, e) && !find_sibling_dll(c, "grycecore").empty()) return c;
    }
    return {};
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

#if defined(_WIN32)
// Hex-encode a digest through the Windows CNG (bcrypt.dll) provider.
std::string cng_hash_hex(LPCWSTR algorithm, const void* data, size_t len,
                         size_t digest_bytes) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, algorithm, nullptr, 0) != 0) return "";
    std::string digest(digest_bytes, '\0');
    const NTSTATUS rc = BCryptHash(alg, nullptr, 0,
                                   reinterpret_cast<PUCHAR>(const_cast<void*>(data)),
                                   static_cast<ULONG>(len),
                                   reinterpret_cast<PUCHAR>(digest.data()),
                                   static_cast<ULONG>(digest.size()));
    BCryptCloseAlgorithmProvider(alg, 0);
    if (rc != 0) return "";
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(digest.size() * 2);
    for (unsigned char c : digest) {
        out.push_back(kHex[c >> 4]);
        out.push_back(kHex[c & 0xF]);
    }
    return out;
}
#else
// ---------------------------------------------------------------------------
// 纯 C++ SHA-256 / SHA-512（FIPS 180-4），供 Linux 等非 Windows 平台使用。
// 与 Windows CNG 行为一致：返回小写十六进制摘要。
// ---------------------------------------------------------------------------
#include <cstdint>

namespace {

inline std::uint32_t rotr32(std::uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }
inline std::uint64_t rotr64(std::uint64_t x, unsigned n) { return (x >> n) | (x << (64 - n)); }

// ---- SHA-256 ----
const std::uint32_t g_sha256_k[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,
    0x923f82a4u,0xab1c5ed5u,0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,
    0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,0xe49b69c1u,0xefbe4786u,
    0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,
    0x06ca6351u,0x14292967u,0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,
    0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,0xa2bfe8a1u,0xa81a664bu,
    0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,
    0x5b9cca4fu,0x682e6ff3u,0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,
    0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u};

void sha256_transform(std::uint32_t s[8], const std::uint8_t* chunk) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (static_cast<std::uint32_t>(chunk[i*4]) << 24)
             | (static_cast<std::uint32_t>(chunk[i*4+1]) << 16)
             | (static_cast<std::uint32_t>(chunk[i*4+2]) << 8)
             | static_cast<std::uint32_t>(chunk[i*4+3]);
    for (int i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotr32(w[i-15],7) ^ rotr32(w[i-15],18) ^ (w[i-15] >> 3);
        const std::uint32_t s1 = rotr32(w[i-2],17) ^ rotr32(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    std::uint32_t a=s[0],b=s[1],c=s[2],d=s[3],e=s[4],f=s[5],g=s[6],hh=s[7];
    for (int i = 0; i < 64; ++i) {
        const std::uint32_t S1 = rotr32(e,6) ^ rotr32(e,11) ^ rotr32(e,25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t t1 = hh + S1 + ch + g_sha256_k[i] + w[i];
        const std::uint32_t S0 = rotr32(a,2) ^ rotr32(a,13) ^ rotr32(a,22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2 = S0 + maj;
        hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    s[0]+=a; s[1]+=b; s[2]+=c; s[3]+=d; s[4]+=e; s[5]+=f; s[6]+=g; s[7]+=hh;
}

std::string sha256_hex_impl(const void* data, size_t len) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    std::uint32_t s[8] = {0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
                          0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u};
    const size_t nblocks = len / 64;
    for (size_t i = 0; i < nblocks; ++i) sha256_transform(s, p + i*64);
    const size_t rem = len % 64;
    const std::uint64_t bitlen = static_cast<std::uint64_t>(len) * 8;
    std::uint8_t tail[128] = {0};
    size_t t = 0;
    if (rem) { std::memcpy(tail, p + nblocks*64, rem); t = rem; }
    tail[t++] = 0x80;
    while (t % 64 != 56) tail[t++] = 0;
    for (int i = 7; i >= 0; --i) tail[t++] = static_cast<std::uint8_t>(bitlen >> (i*8));
    for (size_t off = 0; off < t; off += 64) sha256_transform(s, tail + off);
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (int i = 0; i < 8; ++i)
        for (int shift = 28; shift >= 0; shift -= 4)
            out.push_back(hex[(s[i] >> shift) & 0xF]);
    return out;
}

// ---- SHA-512 ----
const std::uint64_t g_sha512_k[80] = {
    0x428a2f98d728ae22u,0x7137449123ef65cdu,0xb5c0fbcfec4d3b2fu,0xe9b5dba58189dbbcu,
    0x3956c25bf348b538u,0x59f111f1b605d019u,0x923f82a4af194f9bu,0xab1c5ed5da6d8118u,
    0xd807aa98a3030242u,0x12835b0145706fbeu,0x243185be4ee4b28cu,0x550c7dc3d5ffb4e2u,
    0x72be5d74f27b896fu,0x80deb1fe3b1696b1u,0x9bdc06a725c71235u,0xc19bf174cf692694u,
    0xe49b69c19ef14ad2u,0xefbe4786384f25e3u,0x0fc19dc68b8cd5b5u,0x240ca1cc77ac9c65u,
    0x2de92c6f592b0275u,0x4a7484aa6ea6e483u,0x5cb0a9dcbd41fbd4u,0x76f988da831153b5u,
    0x983e5152ee66dfabu,0xa831c66d2db43210u,0xb00327c898fb213fu,0xbf597fc7beef0ee4u,
    0xc6e00bf33da88fc2u,0xd5a79147930aa725u,0x06ca6351e003826fu,0x142929670a0e6e70u,
    0x27b70a8546d22ffcu,0x2e1b21385c26c926u,0x4d2c6dfc5ac42aedu,0x53380d139d95b3dfu,
    0x650a73548baf63deu,0x766a0abb3c77b2a8u,0x81c2c92e47edaee6u,0x92722c851482353bu,
    0xa2bfe8a14cf10364u,0xa81a664bbc423001u,0xc24b8b70d0f89791u,0xc76c51a30654be30u,
    0xd192e819d6ef5218u,0xd69906245565a910u,0xf40e35855771202au,0x106aa07032bbd1b8u,
    0x19a4c116b8d2d0c8u,0x1e376c085141ab53u,0x2748774cdf8eeb99u,0x34b0bcb5e19b48a8u,
    0x391c0cb3c5c95a63u,0x4ed8aa4ae3418acbu,0x5b9cca4f7763e373u,0x682e6ff3d6b2b8a3u,
    0x748f82ee5defb2fcu,0x78a5636f43172f60u,0x84c87814a1f0ab72u,0x8cc702081a6439ecu,
    0x90befffa23631e28u,0xa4506cebde82bde9u,0xbef9a3f7b2c67915u,0xc67178f2e372532bu,
    0xca273eceea26619cu,0xd186b8c721c0c207u,0xeada7dd6cde0eb1eu,0xf57d4f7fee6ed178u,
    0x06f067aa72176fbau,0x0a637dc5a2c898a6u,0x113f9804bef90daeu,0x1b710b35131c471bu,
    0x28db77f523047d84u,0x32caab7b40c72493u,0x3c9ebe0a15c9bebcu,0x431d67c49c100d4cu,
    0x4cc5d4becb3e42b6u,0x597f299cfc657e2au,0x5fcb6fab3ad6faecu,0x6c44198c4a475817u};

void sha512_transform(std::uint64_t s[8], const std::uint8_t* chunk) {
    std::uint64_t w[80];
    for (int i = 0; i < 16; ++i)
        w[i] = (static_cast<std::uint64_t>(chunk[i*8]) << 56)
             | (static_cast<std::uint64_t>(chunk[i*8+1]) << 48)
             | (static_cast<std::uint64_t>(chunk[i*8+2]) << 40)
             | (static_cast<std::uint64_t>(chunk[i*8+3]) << 32)
             | (static_cast<std::uint64_t>(chunk[i*8+4]) << 24)
             | (static_cast<std::uint64_t>(chunk[i*8+5]) << 16)
             | (static_cast<std::uint64_t>(chunk[i*8+6]) << 8)
             | static_cast<std::uint64_t>(chunk[i*8+7]);
    for (int i = 16; i < 80; ++i) {
        const std::uint64_t s0 = rotr64(w[i-15],1) ^ rotr64(w[i-15],8) ^ (w[i-15] >> 7);
        const std::uint64_t s1 = rotr64(w[i-2],19) ^ rotr64(w[i-2],61) ^ (w[i-2] >> 6);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    std::uint64_t a=s[0],b=s[1],c=s[2],d=s[3],e=s[4],f=s[5],g=s[6],hh=s[7];
    for (int i = 0; i < 80; ++i) {
        const std::uint64_t S1 = rotr64(e,14) ^ rotr64(e,18) ^ rotr64(e,41);
        const std::uint64_t ch = (e & f) ^ (~e & g);
        const std::uint64_t t1 = hh + S1 + ch + g_sha512_k[i] + w[i];
        const std::uint64_t S0 = rotr64(a,28) ^ rotr64(a,34) ^ rotr64(a,39);
        const std::uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint64_t t2 = S0 + maj;
        hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    s[0]+=a; s[1]+=b; s[2]+=c; s[3]+=d; s[4]+=e; s[5]+=f; s[6]+=g; s[7]+=hh;
}

std::string sha512_hex_impl(const void* data, size_t len) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    std::uint64_t s[8] = {0x6a09e667f3bcc908u,0xbb67ae8584caa73bu,0x3c6ef372fe94f82bu,
                          0xa54ff53a5f1d36f1u,0x510e527fade682d1u,0x9b05688c2b3e6c1fu,
                          0x1f83d9abfb41bd6bu,0x5be0cd19137e2179u};
    const size_t nblocks = len / 128;
    for (size_t i = 0; i < nblocks; ++i) sha512_transform(s, p + i*128);
    const size_t rem = len % 128;
    const std::uint64_t bitlen = static_cast<std::uint64_t>(len) * 8;
    std::uint8_t tail[256] = {0};
    size_t t = 0;
    if (rem) { std::memcpy(tail, p + nblocks*128, rem); t = rem; }
    tail[t++] = 0x80;
    while (t % 128 != 112) tail[t++] = 0;
    // 128-bit length: high word stays 0 (tail zero-initialized)
    for (int i = 7; i >= 0; --i) tail[t++] = static_cast<std::uint8_t>(bitlen >> (i*8));
    for (size_t off = 0; off < t; off += 128) sha512_transform(s, tail + off);
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(128);
    for (int i = 0; i < 8; ++i)
        for (int shift = 60; shift >= 0; shift -= 4)
            out.push_back(hex[(s[i] >> shift) & 0xF]);
    return out;
}

} // namespace
#endif

std::string sha256_hex(const void* data, size_t len) {
#if defined(_WIN32)
    return cng_hash_hex(BCRYPT_SHA256_ALGORITHM, data, len, 32);
#else
    return sha256_hex_impl(data, len);
#endif
}

std::string sha512_hex(const void* data, size_t len) {
#if defined(_WIN32)
    return cng_hash_hex(BCRYPT_SHA512_ALGORITHM, data, len, 64);
#else
    return sha512_hex_impl(data, len);
#endif
}

std::string read_file_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Locate the directory that holds the MSVC CRT runtime DLLs for the given
// configuration. Prefers the VS install recorded in the build's
// CMakeCache.txt, then scans common VS install locations, and finally falls
// back to System32 (the VC++ redistributable is present there on machines
// that build with MSVC).
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

// Copy the MSVC CRT runtime DLLs into runtime/. When the VS redist folder is
// found, all of its DLLs are copied (it contains exactly the redistributable
// CRT set). Debug builds additionally need the debug Universal CRT
// (ucrtbased.dll) from the Windows Kits / System32.
bool copy_msvc_runtime(const fs::path& build_dir, bool debug,
                       const fs::path& runtime_dir,
                       const fs::path& out_dir,
                       std::vector<std::string>& copied) {
    const fs::path src_dir = find_msvc_crt_dir(build_dir, debug);
    size_t found = 0;
    std::error_code ec;

    // The CRT is statically imported by the exe at process start, i.e. BEFORE
    // main() can run SetDllDirectoryW("runtime"). It must therefore sit next to
    // the exe (out_dir), not only in runtime/. Keep both copies so the runtime/
    // folder stays complete for DLLs resolved later (harmless duplication).
    auto copy_crt = [&](const fs::path& src, const std::string& dll_name) {
        std::error_code ec;
        fs::copy_file(src, runtime_dir / dll_name, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            std::cerr << "[grycegc] ERROR: failed to copy MSVC runtime " << src << "\n";
            return false;
        }
        fs::copy_file(src, out_dir / dll_name, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            std::cerr << "[grycegc] ERROR: failed to copy MSVC runtime to root " << src << "\n";
            return false;
        }
        copied.push_back(dll_name);
        ++found;
        return true;
    };

    // Prefer the VS redist folder: copy every DLL it contains.
    bool from_redist = src_dir.filename() != fs::path("System32");
    if (from_redist) {
        for (const auto& entry : fs::directory_iterator(src_dir, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            if (entry.path().extension() != ".dll") continue;
            if (!copy_crt(entry.path(), entry.path().filename().string())) return false;
        }
    } else {
        // System32 fallback: copy the common CRT names.
        static const std::vector<std::string> kRelease = {
            "vcruntime140.dll", "vcruntime140_1.dll", "vcruntime140_threads.dll",
            "msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll",
            "msvcp140_atomic_wait.dll", "msvcp140_codecvt_ids.dll",
            "concrt140.dll", "vccorlib140.dll", "vcomp140.dll",
        };
        static const std::vector<std::string> kDebug = {
            "vcruntime140d.dll", "vcruntime140_1d.dll", "vcruntime140_threadsd.dll",
            "msvcp140d.dll", "msvcp140_1d.dll", "msvcp140_2d.dll",
            "msvcp140d_atomic_wait.dll", "msvcp140d_codecvt_ids.dll",
            "concrt140d.dll", "vccorlib140d.dll", "vcomp140d.dll",
        };
        const std::vector<std::string>& names = debug ? kDebug : kRelease;
        for (const std::string& dll : names) {
            const fs::path src = src_dir / dll;
            if (!fs::is_regular_file(src, ec)) continue;
            if (!copy_crt(src, dll)) return false;
        }
    }

    // Debug builds also need the debug Universal CRT (ucrtbased.dll), which
    // lives in the Windows Kits (not the VC redist folder). System32 fallback.
    if (debug) {
        fs::path ucrt_src;
        const fs::path kits = "C:\\Program Files (x86)\\Windows Kits\\10\\bin";
        uint64_t best_version = 0;
        std::error_code ec;
        for (const auto& ver : fs::directory_iterator(kits, ec)) {
            if (!ver.is_directory()) continue;
            uint64_t vnum = 0;
            try {
                vnum = std::stoull(ver.path().filename().string());
            } catch (...) {
                continue;
            }
            const fs::path cand = ver.path() / "x64" / "ucrt" / "ucrtbased.dll";
            if (fs::is_regular_file(cand, ec) && vnum > best_version) {
                best_version = vnum;
                ucrt_src = cand;
            }
        }
        if (ucrt_src.empty()) ucrt_src = fs::path("C:\\Windows\\System32") / "ucrtbased.dll";
        if (fs::is_regular_file(ucrt_src, ec)) {
            copy_crt(ucrt_src, "ucrtbased.dll");
        } else {
            std::cerr << "[grycegc] warning: ucrtbased.dll not found (Debug UCRT missing)\n";
        }
    }

    std::printf("[grycegc] MSVC %s runtime: %zu DLL(s) from %s\n",
                debug ? "Debug" : "Release", found, src_dir.string().c_str());
    return true;
}

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

// Write the game root's single config + metadata file "project.data" (JSON).
// It merges the source project's manifest + runtime settings (project.data,
// backward-compatible with the old project.gproj / project_settings.json) with
// the build metadata that used to live in a separate gdata file: every packaged
// source file (path + SHA-256 + size) -> source records, a 64-byte SHA-512 key
// derived from those records, and the ChaCha20 .gpkg decryption key
// (enc_key_hex). project.data is the ONLY state file in the game root; the
// runtime reads both settings and the decryption key from it.
bool write_project_data(const fs::path& project, const fs::path& out_dir,
                        const std::string& name, const std::string& author,
                        const std::vector<FileEntry>& files,
                        const std::string& enc_key_hex) {
    std::error_code ec;

    // 1) 源项目配置：项目清单 + 运行时设置。优先 project.data；兼容旧
    //    project.gproj（清单）与 project_settings.json（运行时设置），后者并入顶层
    //    （键冲突时按读取顺序后者覆盖前者）。
    nlohmann::json merged = nlohmann::json::object();
    for (const char* cfg : {"project.data", "project.gproj", "project_settings.json"}) {
        const fs::path p = project / cfg;
        if (!fs::is_regular_file(p, ec)) continue;
        try {
            std::ifstream in(p);
            nlohmann::json j = nlohmann::json::parse(in, nullptr, false);  // no throw
            if (j.is_discarded()) {
                std::cerr << "[grycegc] warning: failed to parse " << p << ", skipping\n";
                continue;
            }
            if (j.is_object()) {
                for (auto it = j.begin(); it != j.end(); ++it) merged[it.key()] = it.value();
            }
        } catch (...) {
            std::cerr << "[grycegc] warning: failed to parse " << p << ", skipping\n";
        }
    }
    if (merged.empty()) {
        merged["name"] = name;
    }

    // 2) Source records + 64-byte SHA-512 key（对源文件记录做摘要）。
    std::string records;
    nlohmann::json sources = nlohmann::json::array();
    for (const FileEntry& file : files) {
        const std::string bytes = read_file_bytes(file.source_path);
        const std::string digest = sha256_hex(bytes.data(), bytes.size());
        if (digest.empty()) {
            std::cerr << "[grycegc] ERROR: failed to hash " << file.internal_path << "\n";
            return false;
        }
        records += file.internal_path + ":" +
                   std::to_string(fs::file_size(file.source_path)) + ":" +
                   digest + "\n";
        nlohmann::json rec;
        rec["path"] = file.internal_path;
        rec["sha256"] = digest;
        rec["size"] = fs::file_size(file.source_path);
        sources.push_back(rec);
    }
    const std::string key = sha512_hex(records.data(), records.size());
    if (key.empty()) {
        std::cerr << "[grycegc] ERROR: failed to derive project.data key\n";
        return false;
    }

    std::time_t now = std::time(nullptr);
    char created[64] = {};
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    std::strftime(created, sizeof(created), "%Y-%m-%dT%H:%M:%S", &local);

    // 3) 合并打包元数据（原 gdata 字段）写入同一份 project.data。
    merged["format"] = "gryce_project_data";
    merged["version"] = 1;
    merged["project"] = name;
    merged["author"] = author;
    merged["created"] = created;
    merged["tool"] = "grycegc";
    merged["key_sha512_hex"] = key;                                   // 64 bytes, hex-encoded
    merged["enc_key_hex"] = enc_key_hex;  // ChaCha20 32B 密钥（hex），运行时据此解密 .gpkg
    merged["sources"] = sources;

    const fs::path out = out_dir / "project.data";
    std::ofstream out_fs(out);
    if (!out_fs) {
        std::cerr << "[grycegc] ERROR: failed to write " << out << "\n";
        return false;
    }
    out_fs << merged.dump(2) << "\n";
    if (out_fs.good()) {
        std::printf("[grycegc] project.data: %zu source records + 64-byte SHA-512 key + author '%s'"
                    " (manifest, settings & metadata merged at output root)\n",
                    files.size(), author.c_str());
    }
    return out_fs.good();
}

bool copy_runtime(const fs::path& build_dir, const fs::path& bin_dir, bool debug,
                  const fs::path& out_dir, const fs::path& exe, const std::string& name,
                  std::vector<std::string>& copied) {
    const fs::path runtime_dir = out_dir / "runtime";
    std::error_code ec;
    fs::create_directories(runtime_dir, ec);

    // Detect MinGW by its own runtime DLLs (g++-specific, never produced by
    // MSVC). A MinGW build may still carry plain-named engine DLLs, so the
    // lib-prefix fallback below is not sufficient on its own.
    bool mingw = false;
    for (const char* rt : {"libgcc_s_seh-1.dll", "libstdc++-6.dll"}) {
        if (fs::is_regular_file(bin_dir / rt)) { mingw = true; break; }
    }
    // Engine core DLLs are discovered by prefix in bin_dir (同级扫描), not by a
    // hard-coded full name, so debug/release `d` suffix and `lib` prefix are
    // both picked up automatically.
    const std::vector<std::string> core_prefixes = {
        "grycecore", "grycerenderer", "gryceplatform", "grycephysics",
    };
    for (const std::string& prefix : core_prefixes) {
        std::error_code ec2;
        fs::path src = find_sibling_dll(bin_dir, prefix);
        if (src.empty()) {
            std::cerr << "[grycegc] warning: " << prefix << "*.dll not found in " << bin_dir << "\n";
            continue;
        }
        if (src.filename().string().rfind("lib", 0) == 0) mingw = true;
        const std::string dst_name = src.filename().string();
        fs::copy_file(src, runtime_dir / dst_name, fs::copy_options::overwrite_existing, ec2);
        if (ec2) {
            std::cerr << "[grycegc] ERROR: failed to copy " << src << ": " << ec2.message() << "\n";
            return false;
        }
        copied.push_back(dst_name);
    }
    // GLFW: Debug builds (MSVC *and* MinGW) link glfw3d.dll; Release links
    // glfw3.dll. Prefer the variant matching the build's debug flag so the
    // load-time closure resolves the exact DLL the engine was linked against,
    // falling back to the other variant / a bare "glfw" prefix if unavailable.
    bool glfw_ok = false;
    {
        const char* want = debug ? "glfw3d" : "glfw3";
        std::error_code ge;
        fs::path glfw;
        if (!(fs::is_regular_file(bin_dir / (std::string(want) + ".dll"), ge)))
            glfw = find_sibling_dll(bin_dir, want);
        else
            glfw = bin_dir / (std::string(want) + ".dll");
        if (glfw.empty()) {
            for (const char* prefix : {"glfw3", "glfw"})
                if (!(glfw = find_sibling_dll(bin_dir, prefix)).empty()) break;
        }
        if (!glfw.empty()) {
            const std::string dst = glfw.filename().string();
            std::error_code g2;
            if (fs::copy_file(glfw, runtime_dir / dst, fs::copy_options::overwrite_existing, g2)) {
                copied.push_back(dst);
                glfw_ok = true;
            }
        }
    }
    if (!glfw_ok) {
        std::cerr << "[grycegc] warning: glfw3d.dll/glfw3.dll not found in " << bin_dir << "\n";
    }
    // GLEW: the renderer may link it dynamically (MinGW shared GLEW builds).
    // Harmless to include and avoids a hard DLL_NOT_FOUND at load time.
    copy_file_if_exists(bin_dir, "glew32.dll", runtime_dir, copied);
    // MinGW runtime DLLs (self-contained packages; harmless to include).
    for (const char* rt : {"libgcc_s_seh-1.dll", "libstdc++-6.dll", "libwinpthread-1.dll"}) {
        copy_file_if_exists(bin_dir, rt, runtime_dir, copied);
    }
    // Vulkan shader compiler (shaderc_shared.dll): loaded at runtime, so it is
    // optional for GL-only runtimes. Stage it when the host has a Vulkan SDK so
    // packages that use the Vulkan backend self-compile shaders on first run
    // instead of failing open to (possibly absent) precompiled SPIR-V.
    const fs::path shaderc = find_shaderc_dll();
    if (!shaderc.empty()) {
        std::error_code se;
        fs::copy_file(shaderc, runtime_dir / "shaderc_shared.dll",
                      fs::copy_options::overwrite_existing, se);
        if (se) {
            std::cerr << "[grycegc] warning: failed to copy shaderc_shared.dll: "
                      << se.message() << "\n";
        } else {
            copied.push_back("shaderc_shared.dll");
        }
    }
    // MSVC CRT runtime (release/debug per config). MinGW packages already
    // carry their GCC runtime above; System32 is the fallback source.
    if (!mingw) {
        if (!copy_msvc_runtime(build_dir, debug, runtime_dir, out_dir, copied)) {
            return false;
        }
    }

    // MinGW builds do not support /DELAYLOAD, so the exe imports the engine
    // DLLs at process start BEFORE SetDllDirectoryW("runtime") can run. Their
    // DLLs — and every transitive dependency the engine DLLs import (GLFW
    // variant, GLEW, GCC runtime) — must sit next to the exe, not only in
    // runtime/. Mirror the entire staged runtime folder to the output root so
    // the load-time closure always resolves. The delay-load hook in the
    // template still resolves the redundant runtime/ copies later (harmless).
    if (mingw) {
        std::error_code ec2;
        for (const auto& entry : fs::directory_iterator(runtime_dir, ec2)) {
            if (!entry.is_regular_file(ec2)) continue;
            fs::copy_file(entry.path(), out_dir / entry.path().filename(),
                          fs::copy_options::overwrite_existing, ec2);
        }
    }

    fs::copy_file(exe, out_dir / (name + ".exe"), fs::copy_options::overwrite_existing, ec);
    if (ec) {
        std::cerr << "[grycegc] ERROR: failed to copy " << exe << ": " << ec.message() << "\n";
        return false;
    }
    return true;
}

bool write_bundle(const std::vector<FileEntry>& files, const fs::path& output_path,
                  size_t& entry_count) {
    // 使用 PakWriter（GPAK v4）：随机 Base64 存储名 + manifest 映射逻辑路径；
    // 设置了 32 字节全局密钥时对 data 区做 ChaCha20 加密。
    gryce_engine::resources::PakWriter writer;
    bool ok = true;
    for (const FileEntry& file : files) {
        if (!writer.add_file(file.internal_path, file.source_path.string())) {
            std::cerr << "[grycegc] ERROR: PakWriter::add_file('" << file.internal_path << "') failed\n";
            ok = false;
            break;
        }
    }
    if (ok && !writer.write(output_path.string())) {
        std::cerr << "[grycegc] ERROR: PakWriter::write('" << output_path << "') failed\n";
        ok = false;
    }
    if (!ok) return false;

    // 读回验证：打开 + manifest 数量一致 + 每个文件解密环回比对（加密写/解密读必须一致）。
    // 加密包（v4）在验证时用工具的全局 ChaCha20 密钥解密，直接比对恢复内容与源文件字节，
    // 从打包侧即可确认运行时解密（PakReader::read 同一套函数）能还原原始数据。
    gryce_engine::resources::PakReader reader;
    if (!reader.open(output_path.string())) {
        std::cerr << "[grycegc] ERROR: .gpkg verification failed to open " << output_path << "\n";
        return false;
    }
    if (reader.manifest().size() != files.size()) {
        std::cerr << "[grycegc] ERROR: .gpkg verification mismatch in " << output_path << "\n";
        return false;
    }
    for (const FileEntry& file : files) {
        const std::vector<uint8_t> restored = reader.read(file.internal_path);
        std::ifstream ifs(file.source_path, std::ios::binary);
        const std::vector<uint8_t> orig((std::istreambuf_iterator<char>(ifs)),
                                        std::istreambuf_iterator<char>());
        bool content_ok = false;
        if (restored.size() == orig.size()) {
            content_ok = std::equal(restored.begin(), restored.end(), orig.begin());
        }
        if (!content_ok) {
            std::cerr << "[grycegc] ERROR: decrypt mismatch for " << file.internal_path
                      << " in " << output_path << "\n";
            return false;
        }
    }
    entry_count += files.size();
    const uintmax_t size = fs::file_size(output_path);
    std::printf("[grycegc] %s: %zu files, %.2f MiB (random Base64 names, manifest)\n",
                output_path.filename().string().c_str(), reader.manifest().size(),
                static_cast<double>(size) / (1024.0 * 1024.0));
    return true;
}

// 创建标准 GryceGC-A 项目骨架。目录结构见 docs/GryceGC-A.md §2：
//   <dir>/project.data            唯一配置文件（项目清单 + 运行时设置合并于一处，JSON）
//   <dir>/scenes/                 场景 .gesc
//   <dir>/scripts/                 JS 脚本 .js（ES Module，QuickJS 运行时）
//   <dir>/shaders/                 着色器
//   <dir>/models/                  模型
//   <dir>/textures/                贴图
//   <dir>/audio/                   音频
//   <dir>/fonts/                   字体
//   <dir>/tilesets/                Tilemap 瓦片集
// 其中会自动生成一个最小的空主场景 scenes/main.gesc（v2 格式，含一个空根）。
bool create_project_skeleton(const fs::path& dir, const std::string& name,
                             const std::string& window_title) {
    std::error_code ec;
    // 目录必须不存在或为空，避免覆盖已有项目。
    if (fs::exists(dir, ec)) {
        if (fs::is_directory(dir, ec) && !fs::is_empty(dir, ec)) {
            std::cerr << "[grycegc] ERROR: " << dir
                      << " 已存在且非空，拒绝覆盖现有项目\n";
            return false;
        }
        fs::remove_all(dir, ec);
        if (ec) {
            std::cerr << "[grycegc] ERROR: failed to clear " << dir
                      << ": " << ec.message() << "\n";
            return false;
        }
    }

    const std::vector<std::string> subdirs = {
        "scenes", "scripts", "shaders", "models",
        "textures", "audio", "fonts", "tilesets",
    };
    for (const std::string& sub : subdirs) {
        if (!fs::create_directories(dir / sub, ec) && ec) {
            std::cerr << "[grycegc] ERROR: failed to create " << (dir / sub)
                      << ": " << ec.message() << "\n";
            return false;
        }
    }

    // project.data —— 唯一配置文件：项目清单 + 运行时设置合并于一处
    //（原 project_settings.json 的内容并入此文件顶层，作为运行时设置；打包时还会
    // 附加原 gdata 的打包元数据字段。）
    {
        std::string title = window_title.empty() ? name : window_title;
        std::string j = "{\n"
            "  \"name\": \"" + json_escape(name) + "\",\n"
            "  \"version\": \"0.1.0\",\n"
            "  \"engine_version\": \">=0.1.0\",\n"
            "  \"entry_scene\": \"res:/scenes/main.gesc\",\n"
            "  \"physics\": { \"backend_2d\": \"box2d\", \"backend_3d\": \"jolt\" },\n"
            "  \"window\": { \"width\": 1280, \"height\": 720, \"title\": \""
            + json_escape(title) + "\" },\n"
            "  \"render_api\": \"opengl\",\n"
            "  \"hdr\": true,\n"
            "  \"tone_map_mode\": 1,\n"
            "  \"exposure\": 1.0,\n"
            "  \"shadow_enabled\": true,\n"
            "  \"shadow_map_size\": 2048,\n"
            "  \"ambient_r\": 0.2,\n"
            "  \"ambient_g\": 0.22,\n"
            "  \"ambient_b\": 0.26,\n"
            "  \"ibl_intensity\": 1.0,\n"
            "  \"main_scene\": \"res:/scenes/main.gesc\"\n"
            "}\n";
        std::ofstream out(dir / "project.data");
        if (!out) {
            std::cerr << "[grycegc] ERROR: failed to write project.data\n";
            return false;
        }
        out << j;
        if (!out.good()) return false;
    }

    // scenes/main.gesc —— 最小空主场景（v2：单合成根，无落盘实体）
    {
        const char* scene =
            "{\n"
            "  \"version\": 2,\n"
            "  \"name\": \"Main\",\n"
            "  \"entities\": []\n"
            "}\n";
        std::ofstream out(dir / "scenes" / "main.gesc");
        if (!out) {
            std::cerr << "[grycegc] ERROR: failed to write scenes/main.gesc\n";
            return false;
        }
        out << scene;
        if (!out.good()) return false;
    }

    // 各资源目录占位说明（可选，保持空目录在 VCS 中可见）
    for (const std::string& sub : subdirs) {
        if (sub == "scenes") continue;
        const fs::path keep = dir / sub / ".gitkeep";
        std::ofstream out(keep);
        out << "# " << sub << " 资源目录（GryceGC-A）\n";
    }

    std::printf("[grycegc] created GryceGC-A project '%s' at %s\n",
                name.c_str(), dir.string().c_str());
    std::printf("[grycegc]   scenes/main.gesc  -> 主场景（入口）\n");
    std::printf("[grycegc]   project.data     -> 唯一配置文件（清单 + 运行时设置）\n");
    std::printf("[grycegc] 下一步: 将场景/脚本/资源放入对应分类目录，然后\n");
    std::printf("[grycegc]   GryceGC --project %s --name %s --build-dir build --config Release --out build/game\n",
                dir.string().c_str(), name.c_str());
    return true;
}

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

} // namespace

int main(int argc, char* argv[]) {
    init_utf8_console();
    std::string project, name = "MyGame", build_dir = "build", config = "Release",
                out = "build/game", author;
    std::string init_dir, window_title;
    std::string game_exe;
    bool pak_mode = false;
    bool dev_mode = false;
    std::string assets, output_pak = "game.pak";
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
        } else if (arg == "--init" || arg == "--new") {
            if (const char* v = need("--init")) init_dir = v;
        } else if (arg == "--window-title") {
            if (const char* v = need("--window-title")) window_title = v;
        } else if (arg == "--name") {
            if (const char* v = need("--name")) name = v;
        } else if (arg == "--build-dir") {
            if (const char* v = need("--build-dir")) build_dir = v;
        } else if (arg == "--config") {
            if (const char* v = need("--config")) config = v;
        } else if (arg == "--out") {
            if (const char* v = need("--out")) out = v;
        } else if (arg == "--author") {
            if (const char* v = need("--author")) author = v;
        } else if (arg == "--game") {
            if (const char* v = need("--game")) game_exe = v;
        } else if (arg == "--pak") {
            pak_mode = true;
        } else if (arg == "--assets") {
            if (const char* v = need("--assets")) assets = v;
        } else if (arg == "--output") {
            if (const char* v = need("--output")) output_pak = v;
        } else if (arg == "--dev") {
            dev_mode = true;
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else {
            std::cerr << "[grycegc] ERROR: unknown option " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    // ==========================================================================
    // Init 模式：创建标准 GryceGC-A 项目骨架
    // ==========================================================================
    if (!init_dir.empty()) {
        if (pak_mode || !project.empty()) {
            std::cerr << "[grycegc] ERROR: --init cannot be combined with --project/--pak\n";
            return 1;
        }
        // 项目名默认取目录名（转 PascalCase）；显式 --name 保持原样。
        std::string dir_name = fs::path(init_dir).empty()
            ? init_dir : fs::path(init_dir).filename().generic_string();
        if (name == "MyGame" && !dir_name.empty() && !init_dir.ends_with("\\") &&
            !init_dir.ends_with("/")) {
            name = to_pascal_case(dir_name);  // 目录名缺省即项目名（GryceGC-A 约定）
        }
        return create_project_skeleton(fs::path(init_dir), name, window_title) ? 0 : 1;
    }

    // ==========================================================================
    // Pak 模式：将 assets 目录打包为发布 .pak（JS 字节码 + DSL 加密）
    // ==========================================================================
    if (pak_mode) {
        if (assets.empty()) {
            std::cerr << "[grycegc] ERROR: --pak requires --assets <dir>\n";
            return 1;
        }
        std::error_code ec;
        if (!fs::is_directory(assets, ec)) {
            std::cerr << "[grycegc] ERROR: assets directory not found: " << assets << "\n";
            return 1;
        }
        const std::vector<FileEntry> files = collect_project_files(assets);
        if (files.empty()) {
            std::cerr << "[grycegc] ERROR: no packable resources found in " << assets << "\n";
            return 1;
        }

        // Release 模式需要 QuickJS 编译 JS 字节码
        GryceEngineUtils::script::ScriptVM vm;
        if (!dev_mode && !vm.init()) {
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
            if (!dev_mode && ext == ".js") {
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
            } else if (!dev_mode && ext == ".uif") {
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

        if (!writer.write(output_pak)) {
            std::cerr << "[grycegc] ERROR: PakWriter::write('" << output_pak << "') failed\n";
            return 1;
        }

        // 读回验证
        gryce_engine::resources::PakReader reader;
        if (!reader.open(output_pak) || reader.manifest().size() != files.size()) {
            std::cerr << "[grycegc] ERROR: .pak verification failed for " << output_pak << "\n";
            return 1;
        }
        std::printf("[grycegc] %s: %zu resources, %s\n", output_pak.c_str(), files.size(),
                    dev_mode ? "dev mode (no encryption)" : "release mode (AES-256-GCM)");
        return 0;
    }

    if (project.empty()) {
        std::cerr << "[grycegc] ERROR: --project is required\n";
        print_usage(argv[0]);
        return 1;
    }
    bool debug = config == "Debug";
    if (!debug && config != "Release") {
        std::cerr << "[grycegc] ERROR: --config must be Debug or Release\n";
        return 1;
    }

    const fs::path staging = fs::path(build_dir) / "bin" / config;
    // 优先从 dist 布局读取（GryceEngineUtils/lib 为运行库来源，GryceGame/ 为 exe），
    // build.py / CMake 构建默认会生成 dist；未生成时回退到 flat 输出目录。
    const fs::path dist_dir = staging / "dist";
    const fs::path sdk_lib = dist_dir / "GryceEngineUtils" / "lib";
    const bool has_dist = fs::is_directory(sdk_lib);

    // 运行库 DLL 源码：优先按 --build-dir/--config 推导的路径（dist 或 flat）；
    // 若那里没有引擎 DLL（例如跑的是 dist/GryceGC 里的 exe，却忘带 --config），
    // 则回退按 GryceGC 自身位置做多级同级回溯（本级、上溯若干层的
    // GryceEngineUtils/lib），从而无论从哪个构建布局启动都能发现同级运行库。
    const fs::path exe_src_dir(get_exe_dir());
    fs::path bin_dir = has_dist ? sdk_lib : staging;
    if (find_sibling_dll(bin_dir, "grycecore").empty()) {
        const fs::path auto_dir = find_runtime_dir_near_exe(exe_src_dir);
        if (!auto_dir.empty()) bin_dir = auto_dir;
    }
    // 按实际 DLL（GryceCored.dll 等）推断 Debug/Release，供 MSVC CRT 拷贝使用，
    // 避免与 --config 默认值（Release）错配。
    if (sibling_is_debug(bin_dir)) debug = true;

    // GryceGC 依赖 GryceGame：CMake 在构建时把模板 exe 的精确路径编译进工具
    // （GRYCE_GC_GAME_TEMPLATE），--game 可显式覆盖；仅当两者都缺（独立分发的
    // GryceGC）才回退到目录猜测，正常情况下无需再运行时分多位置查找。
    auto file_exists = [](const fs::path& p) {
        std::error_code e;
        return fs::is_regular_file(p, e);
    };
    fs::path exe = game_exe.empty() ? fs::path() : fs::path(game_exe);
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
    if (!fs::is_directory(project, ec)) {
        std::cerr << "[grycegc] ERROR: project directory not found: " << project << "\n";
        return 1;
    }

    const fs::path out_dir = fs::path(out) / name;
    // Fresh package: remove any previous output for this game first so stale
    // files (old DLLs, removed archives, leftover test data) cannot leak into
    // the new build. The path is the explicit <out>/<name> target.
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
    if (!copy_runtime(build_dir, bin_dir, debug, out_dir, exe, name, copied)) {
        return 1;
    }

    // 2) Content: 每个文件单独打包成一个 .gpkg，包文件名随机 Base64（无逻辑含义），
    //    且 data 区以随机生成的 32 字节密钥做 ChaCha20 加密（密钥最终写入 project.data）。
    const std::vector<FileEntry> files = collect_project_files(project);
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

    // 2.5) Core 默认 shader 兜底：把引擎默认 shader 集（项目未覆盖的）一并打成
    //      独立的 .gpkg 打进 assets/，令空项目/缺失 shader 也能开箱渲染。
    //      —— 打的是源文件，首次运行才由 GL/Vulkan 后端运行时编译。
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
    // 3) project.data：合并源项目清单/运行时设置 + 打包元数据（source records、
    //    64-byte SHA-512 key + author），并写入 .gpkg 解密密钥 enc_key_hex。
    if (author.empty()) {
        const char* user = std::getenv("USERNAME");
        author = (user && user[0]) ? user : "Unknown";
    }
    if (!write_project_data(project, out_dir, name, author, files, enc_key_hex)) {
        return 1;
    }

    std::printf("[grycegc] packaged %s -> %s\n", name.c_str(), out_dir.string().c_str());
    std::printf("[grycegc] %s.exe + runtime/%zu DLLs + assets/%zu %s + project.data\n",
                name.c_str(), copied.size(), assets_count, ".gpkg (project resources + core default shaders)");
    std::printf("[grycegc] run with: %s (project root defaults to exe dir)\n",
                (out_dir / (name + ".exe")).string().c_str());
    return 0;
}
