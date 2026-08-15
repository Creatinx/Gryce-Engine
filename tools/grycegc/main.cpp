// GryceGC - GryceEngine Global Compiler (GryceSPC packaging tool).
//
// Packages a game project's res:// content into one or more .gpkg resource
// archives (GPAK format) and assembles a standalone game directory:
//   <out>/<name>/<name>.exe         template executable
//   <out>/<name>/runtime/           core runtime DLLs
//   <out>/<name>/assets/*.gpkg      game content archives (no raw res/ copy)
//   <out>/<name>/project_settings.json
//                                   raw copy of the runtime settings (the game
//                                   entry reads it from the project root)
//   <out>/<name>/project.gryce      project manifest (documentation)
//   <out>/<name>/gdata              package metadata: source-file records,
//                                   a 64-byte SHA-512 key, author info
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
#include <array>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
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
                       std::vector<std::string>& copied) {
    const fs::path src_dir = find_msvc_crt_dir(build_dir, debug);
    size_t found = 0;

    // Prefer the VS redist folder: copy every DLL it contains.
    bool from_redist = src_dir.filename() != fs::path("System32");
    if (from_redist) {
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(src_dir, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            if (entry.path().extension() != ".dll") continue;
            fs::copy_file(entry.path(), runtime_dir / entry.path().filename(),
                          fs::copy_options::overwrite_existing, ec);
            if (ec) {
                std::cerr << "[grycegc] ERROR: failed to copy MSVC runtime "
                          << entry.path() << "\n";
                return false;
            }
            copied.push_back(entry.path().filename().string());
            ++found;
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
            std::error_code ec;
            const fs::path src = src_dir / dll;
            if (!fs::is_regular_file(src, ec)) continue;
            fs::copy_file(src, runtime_dir / dll, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                std::cerr << "[grycegc] ERROR: failed to copy MSVC runtime " << src << "\n";
                return false;
            }
            copied.push_back(dll);
            ++found;
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
            fs::copy_file(ucrt_src, runtime_dir / "ucrtbased.dll",
                          fs::copy_options::overwrite_existing, ec);
            if (ec) {
                std::cerr << "[grycegc] ERROR: failed to copy ucrtbased.dll\n";
                return false;
            }
            copied.push_back("ucrtbased.dll");
            ++found;
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

// Writes the package metadata file "gdata": records every source file that
// was packaged (path + SHA-256 + size), a 64-byte SHA-512 key derived from
// the source records, and author/project metadata.
bool write_gdata(const fs::path& out_dir, const std::string& name,
                 const std::string& author,
                 const std::vector<FileEntry>& files) {
    std::string records;   // sorted "path:size:sha256" lines -> key input
    std::ostringstream sources;
    sources << "\"sources\": [";
    bool first = true;
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
        if (!first) sources << ", ";
        first = false;
        sources << "{\"path\": \"" << json_escape(file.internal_path)
                << "\", \"sha256\": \"" << digest
                << "\", \"size\": " << fs::file_size(file.source_path) << "}";
    }
    sources << "]";

    // 64-byte key: SHA-512 over the source-file records.
    const std::string key = sha512_hex(records.data(), records.size());
    if (key.empty()) {
        std::cerr << "[grycegc] ERROR: failed to derive gdata key\n";
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

    std::ostringstream gdata;
    gdata << "{\n"
          << "  \"format\": \"gryce_gdata\",\n"
          << "  \"version\": 1,\n"
          << "  \"project\": \"" << json_escape(name) << "\",\n"
          << "  \"author\": \"" << json_escape(author) << "\",\n"
          << "  \"created\": \"" << created << "\",\n"
          << "  \"tool\": \"grycegc\",\n"
          << "  \"key_sha512_hex\": \"" << key << "\",\n"   // 64 bytes, hex-encoded
          << "  " << sources.str() << "\n"
          << "}\n";

    std::ofstream out(out_dir / "gdata", std::ios::binary);
    if (!out) {
        std::cerr << "[grycegc] ERROR: failed to write " << (out_dir / "gdata") << "\n";
        return false;
    }
    out << gdata.str();
    std::printf("[grycegc] gdata: 64-byte SHA-512 key + %zu source records, author '%s'\n",
                files.size(), author.c_str());
    return out.good();
}

bool copy_runtime(const fs::path& build_dir, const fs::path& bin_dir, bool debug,
                  const fs::path& out_dir, const fs::path& exe, const std::string& name,
                  std::vector<std::string>& copied) {
    const fs::path runtime_dir = out_dir / "runtime";
    std::error_code ec;
    fs::create_directories(runtime_dir, ec);

    const std::string suffix = debug ? "d" : "";
    bool mingw = false;
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
            mingw = true;
        }
        const std::string dst_name = src.filename().string();
        fs::copy_file(src, runtime_dir / dst_name, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            std::cerr << "[grycegc] ERROR: failed to copy " << src << ": " << ec.message() << "\n";
            return false;
        }
        copied.push_back(dst_name);
    }
    // GLFW: MSVC Debug builds use glfw3d.dll; MinGW uses plain glfw3.dll.
    bool glfw_ok = false;
    for (const char* glfw : {"glfw3d.dll", "glfw3.dll"}) {
        if (copy_file_if_exists(bin_dir, glfw, runtime_dir, copied)) {
            glfw_ok = true;
            break;
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
    // MSVC CRT runtime (release/debug per config). MinGW packages already
    // carry their GCC runtime above; System32 is the fallback source.
    if (!mingw) {
        if (!copy_msvc_runtime(build_dir, debug, runtime_dir, copied)) {
            return false;
        }
    }

    // MinGW builds do not support /DELAYLOAD, so the exe imports the engine
    // DLLs at process start BEFORE SetDllDirectoryW("runtime") can run. Their
    // DLLs must therefore sit next to the exe, not only in runtime/. Copy the
    // full import set to the output root so packaged MinGW games actually boot.
    if (mingw) {
        const std::array<std::string, 9> root_dlls{
            "libGryceCore" + suffix + ".dll",
            "libGryceRenderer" + suffix + ".dll",
            "libGrycePlatform" + suffix + ".dll",
            "libGrycePhysics" + suffix + ".dll",
            "glfw3.dll",
            "glew32.dll",
            "libgcc_s_seh-1.dll",
            "libstdc++-6.dll",
            "libwinpthread-1.dll"};
        for (const std::string& root_dll : root_dlls) {
            std::error_code sec;
            const fs::path root_src = bin_dir / root_dll;
            if (fs::is_regular_file(root_src, sec)) {
                fs::copy_file(root_src, out_dir / root_dll,
                              fs::copy_options::overwrite_existing, ec);
            }
        }
    }

    fs::copy_file(exe, out_dir / (name + ".exe"), fs::copy_options::overwrite_existing, ec);
    if (ec) {
        std::cerr << "[grycegc] ERROR: failed to copy " << exe << ": " << ec.message() << "\n";
        return false;
    }
    return true;
}

// Copy the project's runtime settings / manifest next to the packaged exe.
// The game entry reads project_settings.json from the project root (exe dir)
// as a real file; it is also packed into config.gpkg, but the raw copy keeps
// the packaged game booting with the correct main scene / settings even when
// bundle extraction is unavailable. project.gryce is copied as documentation.
void copy_project_metadata(const fs::path& project, const fs::path& out_dir) {
    for (const char* name : {"project_settings.json", "project.gryce"}) {
        std::error_code ec;
        const fs::path src = project / name;
        if (!fs::is_regular_file(src, ec)) continue;
        fs::copy_file(src, out_dir / name, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            std::cerr << "[grycegc] ERROR: failed to copy " << src << ": " << ec.message() << "\n";
        } else {
            std::printf("[grycegc] copied %s to output root\n", name);
        }
    }
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
        "  --author <name>   author stored in gdata (default: %USERNAME%)\n"
        "  --single          pack everything into one <name>.gpkg\n",
        argv0);
}

} // namespace

int main(int argc, char* argv[]) {
    std::string project, name = "MyGame", build_dir = "build", config = "Release",
                out = "build/game", author;
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
        } else if (arg == "--author") {
            if (const char* v = need("--author")) author = v;
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
    copy_project_metadata(project, out_dir);

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
        const fs::path bundle_path = assets_dir / (name + suffix + ".gpkg");
        if (!write_bundle(members, bundle_path, total_entries)) {
            return 1;
        }
    }

    // 3) gdata: source-file records + 64-byte SHA-512 key + author metadata.
    if (author.empty()) {
        const char* user = std::getenv("USERNAME");
        author = (user && user[0]) ? user : "Unknown";
    }
    if (!write_gdata(out_dir, name, author, files)) {
        return 1;
    }

    std::printf("[grycegc] packaged %s -> %s\n", name.c_str(), out_dir.string().c_str());
    std::printf("[grycegc] %s.exe + runtime/%zu DLLs + assets/%zu .gpkg (%zu resources) + gdata\n",
                name.c_str(), copied.size(), bundles.size(), total_entries);
    std::printf("[grycegc] run with: %s (project root defaults to exe dir)\n",
                (out_dir / (name + ".exe")).string().c_str());
    return 0;
}
