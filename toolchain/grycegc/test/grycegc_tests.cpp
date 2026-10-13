// ---------------------------------------------------------------------------
// GryceGC 自测试：与 GryceMathTests 同模式，--suite 选择子集
// 子集：
//   cli    参数解析
//   crc32  CRC32 标准向量 + 引擎共享实现
//   pe     自建最小 PE32+ 测试导入表解析
//   aes    AES-256-GCM 往返 + 篡改检测
//   smoke  真实构建树冒烟（如 build/GryceRenderSmoke.exe 存在）
// ---------------------------------------------------------------------------

#include "cli.h"
#include "core/fsutil.h"
#include "core/pe_image.h"
#include "core/dep_resolver.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// 共享 crypto（直接从引擎源码编译进 grycegc_core）
#include "utils/crypto/crc32.h"
#include "resources/aes_gcm.h"

namespace {

int g_failures = 0;
int g_total = 0;
const char* g_suite = "";

#define EXPECT(cond, msg)                                                     \
    do {                                                                      \
        ++g_total;                                                            \
        if (!(cond)) {                                                        \
            ++g_failures;                                                     \
            std::fprintf(stderr, "  [FAIL] %s:%d  %s\n", __FILE__, __LINE__,  \
                         (msg));                                              \
        }                                                                     \
    } while (0)

// ---------------------------------------------------------------------------
// 合成 PE32+ 最小结构
// 目的：测试 pe_image::import_dll_names 与 dep_resolver 的解析路径
// 局限：仅做导入表正确性校验；不通过 Windows 加载器加载。
// ---------------------------------------------------------------------------

struct PE32PlusBuilder {
    std::vector<uint8_t> data;
    uint32_t section_rva = 0x200;
    uint32_t section_file_offset = 0x200;
    uint32_t section_size = 0x400;
    std::vector<std::string> dll_names;

    PE32PlusBuilder() { data.assign(0x1000, 0); }

    void write(uint32_t off, const void* p, size_t n) {
        std::memcpy(data.data() + off, p, n);
    }
    template<typename T> void write_t(uint32_t off, T v) { write(off, &v, sizeof(v)); }

    void build() {
        // DOS
        write_t<uint16_t>(0, 0x5A4D);  // "MZ"
        write_t<uint32_t>(0x3C, 0x40); // e_lfanew
        // PE
        write(0x40, "PE\0\0", 4);
        const uint32_t coff = 0x44;
        write_t<uint16_t>(coff + 0, 0x8664);   // machine: AMD64
        write_t<uint16_t>(coff + 2, 1);        // NumberOfSections
        write_t<uint16_t>(coff + 16, 240);     // SizeOfOptionalHeader (PE32+ standard)
        write_t<uint16_t>(coff + 18, 0x0002);  // Characteristics: EXECUTABLE_IMAGE
        const uint32_t opt = coff + 20;
        // Optional header (PE32+)
        write_t<uint16_t>(opt + 0, 0x20B);     // magic
        write_t<uint8_t>(opt + 2, 14);         // MajorLinkerVersion
        write_t<uint32_t>(opt + 4, 0);         // SizeOfCode
        write_t<uint32_t>(opt + 16, 0x1000);   // AddressOfEntryPoint
        write_t<uint32_t>(opt + 20, section_rva);  // BaseOfCode
        // data directories start at opt + 112 for PE32+
        // dir[1] = import table RVA+size at opt+112+8
        write_t<uint32_t>(opt + 108, 16);  // NumberOfRvaAndSizes（解析器要求 >= 2）
        // Build section data
        // 头占 opt+240 = 0x58+240 = 0x148; section header 40 bytes -> 0x170
        // Pad to 0x200
        // Section data layout (file offset 0x200 + RVA 0x200):
        //   - 0x000: import descriptors (20 bytes each, 1 + null terminator per DLL? No: per DLL is 1 IMAGE_IMPORT_DESCRIPTOR, with null terminator once at end)
        // We support N DLLs: N IMAGE_IMPORT_DESCRIPTOR (20B each) + 1 null terminator (20B).
        // Then for each: ILT (array of u64 + null terminator = 8 bytes) and name string.
        uint32_t cur = section_file_offset;
        const uint32_t import_table_rva = section_rva;
        const uint32_t import_count = static_cast<uint32_t>(dll_names.size());
        std::vector<uint32_t> ilt_rvas(import_count);
        std::vector<uint32_t> name_rvas(import_count);
        const uint32_t desc_bytes = (import_count + 1) * 20;
        cur += desc_bytes;
        // After descriptors, lay out ILTs and names
        for (uint32_t i = 0; i < import_count; ++i) {
            ilt_rvas[i] = section_rva + (cur - section_file_offset);
            cur += 16;  // one u64 hint-name entry (8) + null (8) = 16
        }
        for (uint32_t i = 0; i < import_count; ++i) {
            name_rvas[i] = section_rva + (cur - section_file_offset);
            cur += static_cast<uint32_t>(dll_names[i].size()) + 1;
        }
        const uint32_t section_data_size = cur - section_file_offset;
        // Write IMAGE_IMPORT_DESCRIPTORs
        uint32_t desc_off = section_file_offset;
        for (uint32_t i = 0; i < import_count; ++i) {
            write_t<uint32_t>(desc_off + 0, ilt_rvas[i]);   // OriginalFirstThunk
            write_t<uint32_t>(desc_off + 4, 0);             // TimeDateStamp
            write_t<uint32_t>(desc_off + 8, 0);             // ForwarderChain
            write_t<uint32_t>(desc_off + 12, name_rvas[i]); // Name
            write_t<uint32_t>(desc_off + 16, 0);            // FirstThunk
            desc_off += 20;
        }
        // Null terminator (20 zero bytes) at desc_off
        // Write ILTs
        for (uint32_t i = 0; i < import_count; ++i) {
            write_t<uint64_t>(ilt_rvas[i] - section_rva + section_file_offset, 0x8000000000000001ULL);
        }
        // Write name strings
        for (uint32_t i = 0; i < import_count; ++i) {
            const auto off = name_rvas[i] - section_rva + section_file_offset;
            std::memcpy(data.data() + off, dll_names[i].data(), dll_names[i].size());
            data[off + dll_names[i].size()] = 0;
        }
        // Section header
        const uint32_t sh = opt + 240;
        write(sh, ".text\0\0\0", 8);
        write_t<uint32_t>(sh + 8, section_data_size);     // VirtualSize
        write_t<uint32_t>(sh + 12, section_rva);          // VirtualAddress
        write_t<uint32_t>(sh + 16, section_data_size);     // SizeOfRawData
        write_t<uint32_t>(sh + 20, section_file_offset);   // PointerToRawData
        // Optional header data directories: import at index 1
        write_t<uint32_t>(opt + 112 + 8, import_table_rva);
        write_t<uint32_t>(opt + 112 + 12, desc_bytes);
    }
};

// ---------------------------------------------------------------------------
// suite: cli
// ---------------------------------------------------------------------------
void run_cli() {
    std::printf("[suite cli]\n");
    using namespace grycegc;
    {
        const char* av[] = {"GryceGC", "verify", "--package", "dist/X", "--dll-folder", "dll"};
        std::string cmd, err;
        Options opts;
        EXPECT(parse_argv(6, av, cmd, opts, err), "basic parse");
        EXPECT(cmd == "verify", "command name");
        EXPECT(opts.get("package") == "dist/X", "package value");
        EXPECT(opts.get("dll-folder") == "dll", "dll-folder value");
        EXPECT(opts.is_flag("") == false, "no flag");  // is_flag(empty) false
    }
    {
        const char* av[] = {"GryceGC", "package", "--clean", "--name=Eng", "--api", "vulkan", "extra_positional"};
        std::string cmd, err;
        Options opts;
        EXPECT(parse_argv(7, av, cmd, opts, err), "parse2");
        EXPECT(cmd == "package", "cmd");
        EXPECT(opts.is_flag("clean"), "clean flag");
        EXPECT(opts.get("name") == "Eng", "name=value");
        EXPECT(opts.get("api") == "vulkan", "api value");
        EXPECT(opts.positionals.size() == 1 && opts.positionals[0] == "extra_positional", "positional");
    }
    {
        const char* av[] = {"GryceGC", "run", "--", "--play", "-v"};
        std::string cmd, err;
        Options opts;
        EXPECT(parse_argv(5, av, cmd, opts, err), "parse3");
        EXPECT(cmd == "run", "cmd");
        EXPECT(opts.passthrough.size() == 2 && opts.passthrough[0] == "--play" && opts.passthrough[1] == "-v", "passthrough");
    }
    {
        const char* av[] = {"GryceGC", "deps", "--key", "-5"};
        std::string cmd, err;
        Options opts;
        EXPECT(parse_argv(4, av, cmd, opts, err), "negative value treated as flag");
        EXPECT(opts.is_flag("key"), "key is flag when value starts with -");
    }
    {
        const char* av[] = {"GryceGC", "deps", "--key=-5"};
        std::string cmd, err;
        Options opts;
        EXPECT(parse_argv(3, av, cmd, opts, err), "negative via =");
        EXPECT(opts.get("key") == "-5", "key=-5");
    }
    {
        const char* av[] = {"GryceGC", "foo", "-x"};
        std::string cmd, err;
        Options opts;
        EXPECT(!parse_argv(3, av, cmd, opts, err), "short option rejected");
        EXPECT(!err.empty(), "error non-empty");
    }
    {
        const char* av[] = {"GryceGC", "deps", "--unknown"};
        std::string cmd, err;
        Options opts;
        EXPECT(parse_argv(3, av, cmd, opts, err), "unknown option = flag, no error");
        EXPECT(opts.is_flag("unknown"), "unknown captured as flag (commands decide)");
    }
    {
        const char* av[] = {"GryceGC", "verify", "--pkg"};
        std::string cmd, err;
        Options opts;
        EXPECT(parse_argv(3, av, cmd, opts, err), "flag --pkg");
        EXPECT(opts.is_flag("pkg"), "flag form");
    }
    std::printf("[suite cli] done\n");
}

// ---------------------------------------------------------------------------
// suite: crc32
// ---------------------------------------------------------------------------
void run_crc32() {
    std::printf("[suite crc32]\n");
    // 标准测试向量："123456789" → 0xCBF43926
    const char* s = "123456789";
    auto c = gryce_engine::utils::crc32(reinterpret_cast<const uint8_t*>(s), 9);
    EXPECT(c == 0xCBF43926u, "crc32 known vector");
    std::printf("[suite crc32] done\n");
}

// ---------------------------------------------------------------------------
// suite: pe
// ---------------------------------------------------------------------------
void run_pe() {
    std::printf("[suite pe]\n");
    using namespace grycegc;
    PE32PlusBuilder b;
    b.dll_names = {"USER32.DLL", "GryceCore.dll", "WINMM.dll"};
    b.build();
    auto tmp = temp_dir() / "grycegc_test_pe32plus.bin";
    EXPECT(write_binary(tmp, b.data.data(), b.data.size()), "write pe");
    auto names = pe::import_dll_names(tmp);
    std::vector<std::string> lowers;
    for (auto& n : names) {
        std::string l = n;
        std::transform(l.begin(), l.end(), l.begin(), ::tolower);
        lowers.push_back(l);
    }
    EXPECT(lowers.size() == 3, "3 imports parsed");
    bool has_user32 = false, has_core = false, has_winmm = false;
    for (auto& n : lowers) {
        if (n == "user32.dll") has_user32 = true;
        if (n == "grycecore.dll") has_core = true;
        if (n == "winmm.dll") has_winmm = true;
    }
    EXPECT(has_user32 && has_core && has_winmm, "names match");

    // 系统 DLL 过滤（user32 在系统集合内）
    EXPECT(is_system_dll("kernel32.dll"), "system kernel32");
    EXPECT(is_system_dll("api-ms-win-crt-runtime-l1-1-0.dll"), "system api-ms-win-*");
    EXPECT(!is_system_dll("grycecore.dll"), "engine core not system");
    EXPECT(is_system_dll("user32.dll"), "user32 is system");

    // 分类
    EXPECT(grycegc::classify("grycecore.dll") == DepCategory::Core, "core");
    EXPECT(grycegc::classify("user32.dll") == DepCategory::Dependency, "user32 dependency");
    EXPECT(grycegc::classify("glew32.dll") == DepCategory::Dependency, "glew dependency");

    // Trailer roundtrip
    pe::Trailer tr1;
    tr1.payload = "hello-gryce";
    EXPECT(pe::write_trailer(tmp, tr1), "write trailer");
    pe::Trailer tr2;
    EXPECT(pe::read_trailer(tmp, tr2), "read trailer");
    EXPECT(tr2.payload == tr1.payload, "trailer payload matches");
    // Replace
    tr1.payload = "updated";
    EXPECT(pe::write_trailer(tmp, tr1), "replace trailer");
    EXPECT(pe::read_trailer(tmp, tr2) && tr2.payload == "updated", "trailer replaced");
    // Strip
    auto stripped = pe::strip_trailer(tmp);
    EXPECT(!stripped.empty() && stripped.size() < b.data.size() + 50, "stripped smaller");

    std::filesystem::remove(tmp);
    std::printf("[suite pe] done\n");
}

// ---------------------------------------------------------------------------
// suite: aes（引擎 aes_gcm 共享往返）
// ---------------------------------------------------------------------------
void run_aes() {
    std::printf("[suite aes]\n");
    using namespace gryce_engine::resources;
    uint8_t key[32];
    for (int i = 0; i < 32; ++i) key[i] = static_cast<uint8_t>(i * 7 + 1);
    std::vector<uint8_t> pt(1024);
    for (size_t i = 0; i < pt.size(); ++i) pt[i] = static_cast<uint8_t>(i & 0xFF);
    const char* aad_str = "gryce-meta-1";
    std::vector<uint8_t> aad(aad_str, aad_str + std::strlen(aad_str));

    std::vector<uint8_t> blob;
    EXPECT(aes_gcm_encrypt_buf(key, aad.data(), aad.size(), pt.data(), pt.size(), blob),
           "encrypt");
    EXPECT(blob.size() == 12 + pt.size() + 16, "blob size = iv+ct+tag");
    std::vector<uint8_t> dec;
    EXPECT(aes_gcm_decrypt_buf(key, aad.data(), aad.size(), blob.data(), blob.size(), dec),
           "decrypt roundtrip");
    EXPECT(dec == pt, "roundtrip bytes match");
    // 篡改密文：tag 校验失败
    blob[20] ^= 0x01;
    std::vector<uint8_t> dec2;
    EXPECT(!aes_gcm_decrypt_buf(key, aad.data(), aad.size(), blob.data(), blob.size(), dec2),
           "tamper detected");
    EXPECT(dec2.empty(), "tampered decrypt clears plaintext");
    // AAD 不一致
    std::string wrong = "wrong";
    std::vector<uint8_t> dec3;
    EXPECT(!aes_gcm_decrypt_buf(key,
                                reinterpret_cast<const uint8_t*>(wrong.data()),
                                wrong.size(),
                                blob.data(), blob.size(), dec3),
           "AAD mismatch detected");
    std::printf("[suite aes] done\n");
}

// ---------------------------------------------------------------------------
// suite: smoke（依赖 build/GryceRenderSmoke.exe；不存在则跳过但视为通过）
// ---------------------------------------------------------------------------
void run_smoke() {
    std::printf("[suite smoke]\n");
    namespace fs = std::filesystem;
    std::vector<fs::path> candidates;
    // 1) 显式环境变量
    if (const char* env = std::getenv("GRYCEGC_SMOKE_BUILD_DIR")) {
        candidates.emplace_back(env);
    }
    // 2) CWD 相对路径
    candidates.emplace_back("cmake-build-debug");
    candidates.emplace_back("build");
    // 3) 从测试可执行文件位置反推引擎根 build（grycegc/build → 引擎根/build）
    char exe_path_buf[MAX_PATH];
    if (GetModuleFileNameA(nullptr, exe_path_buf, MAX_PATH) > 0) {
        const fs::path self(exe_path_buf);
        candidates.emplace_back(self.parent_path() / ".." / ".." / ".." / "build");
    }
    fs::path build_dir;
    for (const auto& c : candidates) {
        if (fs::is_directory(c)) { build_dir = fs::weakly_canonical(c); break; }
    }
    if (build_dir.empty()) {
        std::printf("  [skip] no build directory; build engine first to enable smoke\n");
        return;
    }
    fs::path exe = build_dir / "GryceRenderSmoke.exe";
    if (!fs::is_regular_file(exe)) {
        // 也可能装在子目录里
        std::error_code ec;
        bool found = false;
        for (auto it = fs::recursive_directory_iterator(
                 build_dir, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator();
             it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            if (it->path().filename() == "GryceRenderSmoke.exe") {
                exe = it->path();
                found = true;
                break;
            }
        }
        if (!found) {
            std::printf("  [skip] GryceRenderSmoke.exe not found under %s\n",
                        build_dir.string().c_str());
            return;
        }
    }
    grycegc::DepResolver r(build_dir, std::vector<fs::path>{exe.parent_path()});
    auto result = r.resolve(exe);
    bool has_core = false;
    for (const auto& n : result.found) {
        if (grycegc::classify(n.name_lower) == grycegc::DepCategory::Core) {
            has_core = true;
        }
    }
    EXPECT(has_core, "smoke: closure includes an engine core DLL");
    std::printf("  closure size: found=%zu missing=%zu\n",
                result.found.size(), result.missing.size());
    std::printf("[suite smoke] done\n");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3 || std::strcmp(argv[1], "--suite") != 0) {
        std::fprintf(stderr, "usage: %s --suite <cli|crc32|pe|aes|smoke>\n", argv[0]);
        return 2;
    }
    g_suite = argv[2];
    if (!std::strcmp(g_suite, "cli")) run_cli();
    else if (!std::strcmp(g_suite, "crc32")) run_crc32();
    else if (!std::strcmp(g_suite, "pe")) run_pe();
    else if (!std::strcmp(g_suite, "aes")) run_aes();
    else if (!std::strcmp(g_suite, "smoke")) run_smoke();
    else {
        std::fprintf(stderr, "unknown suite: %s\n", g_suite);
        return 2;
    }
    std::printf("[%s] %d/%d passed\n", g_suite, g_total - g_failures, g_total);
    return g_failures == 0 ? 0 : 1;
}
