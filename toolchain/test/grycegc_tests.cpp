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
#include "commands/commands.h"
#include "core/fsutil.h"
#include "core/meta.h"
#include "core/packager.h"
#include "core/pe_image.h"
#include "core/dep_resolver.h"
#include "core/web_pack.h"

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
#include "resources/gpack_bundle.h"
#include "resources/pak_bundle.h"

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
    // 3) 从测试可执行文件位置反推引擎根 build（toolchain/build 或 build/toolchain，
    //    两种布局同深，../../build 均指向引擎根/build）
    char exe_path_buf[MAX_PATH];
    if (GetModuleFileNameA(nullptr, exe_path_buf, MAX_PATH) > 0) {
        const fs::path self(exe_path_buf);
        candidates.emplace_back(self.parent_path() / ".." / ".." / "build");
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

// ---------------------------------------------------------------------------
// suite: crypto（encrypt/decrypt/keygen 命令级往返 + 篡改检测）
// ---------------------------------------------------------------------------
void run_crypto() {
    using namespace grycegc;
    std::printf("[suite crypto]\n");
    namespace fs = std::filesystem;
    const fs::path tmp = temp_dir() / "grycegc_test_crypto";
    fs::remove_all(tmp);
    mkdir_p(tmp);

    const fs::path pt = tmp / "pt.bin";
    std::string plain = "GryceGC crypto roundtrip payload 0123456789abcdef";
    EXPECT(write_binary(pt, plain), "write plaintext");

    Options ko;
    ko.flags.insert("hex");
    // keygen 走 stdout；用 set 形式直接构造 Options 不可行，改为进程级不可用——
    // 这里直接生成密钥字节写文件，再让命令读 @file。
    uint8_t key_bytes[32];
    for (int i = 0; i < 32; ++i) key_bytes[i] = static_cast<uint8_t>(i * 13 + 7);
    const fs::path key_file = tmp / "key.bin";
    EXPECT(write_binary(key_file, key_bytes, sizeof(key_bytes)), "write key file");
    const std::string key_spec = "@" + key_file.string();

    auto make_opts = [&](const char* sub, const std::string& in, const std::string& out,
                         const std::string& algo, const std::string& aad,
                         bool raw) {
        Options o;
        if (sub) o.positionals.push_back(sub);
        o.single["in"] = in;
        o.single["out"] = out;
        o.single["key"] = key_spec;
        if (!algo.empty()) o.single["algo"] = algo;
        if (!aad.empty()) o.single["aad"] = aad;
        if (raw) o.flags.insert("raw");
        return o;
    };

    // AES-GCM 容器往返
    const fs::path ct1 = tmp / "ct1.bin", rt1 = tmp / "rt1.bin";
    EXPECT(cmd_encrypt(make_opts(nullptr, pt.string(), ct1.string(), "aes-gcm", "aad-1",
                                 false)) == 0,
           "encrypt aes-gcm");
    EXPECT(cmd_decrypt(make_opts(nullptr, ct1.string(), rt1.string(), "aes-gcm", "aad-1",
                                 false)) == 0,
           "decrypt aes-gcm");
    auto rt1_bytes = read_all(rt1);
    EXPECT(rt1_bytes.size() == plain.size() &&
               std::memcmp(rt1_bytes.data(), plain.data(), plain.size()) == 0,
           "aes-gcm roundtrip content");

    // ChaCha20 容器往返
    const fs::path ct2 = tmp / "ct2.bin", rt2 = tmp / "rt2.bin";
    EXPECT(cmd_encrypt(make_opts(nullptr, pt.string(), ct2.string(), "chacha20", "",
                                 false)) == 0,
           "encrypt chacha20");
    EXPECT(cmd_decrypt(make_opts(nullptr, ct2.string(), rt2.string(), "chacha20", "",
                                 false)) == 0,
           "decrypt chacha20");
    auto rt2_bytes = read_all(rt2);
    EXPECT(rt2_bytes.size() == plain.size() &&
               std::memcmp(rt2_bytes.data(), plain.data(), plain.size()) == 0,
           "chacha20 roundtrip content");

    // raw（引擎兼容）往返
    const fs::path ct3 = tmp / "ct3.bin", rt3 = tmp / "rt3.bin";
    EXPECT(cmd_encrypt(make_opts(nullptr, pt.string(), ct3.string(), "aes-gcm", "",
                                 true)) == 0,
           "encrypt raw");
    auto ct3_bytes = read_all(ct3);
    EXPECT(ct3_bytes.size() == 12 + plain.size() + 16, "raw layout iv||ct||tag");
    EXPECT(cmd_decrypt(make_opts(nullptr, ct3.string(), rt3.string(), "aes-gcm", "",
                                 false)) == 0,
           "decrypt raw");
    auto rt3_bytes = read_all(rt3);
    EXPECT(rt3_bytes.size() == plain.size() &&
               std::memcmp(rt3_bytes.data(), plain.data(), plain.size()) == 0,
           "raw roundtrip content");

    // 篡改检测：改 1 字节密文必须解密失败
    auto bad = read_all(ct1);
    bad[20] ^= 0xFF;
    const fs::path ct_bad = tmp / "ct_bad.bin";
    EXPECT(write_binary(ct_bad, bad.data(), bad.size()), "write tampered");
    EXPECT(cmd_decrypt(make_opts(nullptr, ct_bad.string(), (tmp / "rt_bad.bin").string(),
                                 "aes-gcm", "aad-1", false)) != 0,
           "tamper detected");

    // 错误密钥必须失败
    uint8_t other[32];
    for (int i = 0; i < 32; ++i) other[i] = ~key_bytes[i];
    const fs::path key2 = tmp / "key2.bin";
    EXPECT(write_binary(key2, other, sizeof(other)), "write key2");
    Options o2 = make_opts(nullptr, ct1.string(), (tmp / "rt_k2.bin").string(), "aes-gcm",
                           "aad-1", false);
    o2.single["key"] = "@" + key2.string();
    EXPECT(cmd_decrypt(o2) != 0, "wrong key rejected");

    // keygen：--out 文件长度与 --bytes 一致
    const fs::path kg = tmp / "kg.bin";
    Options g;
    g.single["bytes"] = "48";
    g.single["out"] = kg.string();
    EXPECT(cmd_keygen(g) == 0 && read_all(kg).size() == 48, "keygen 48 bytes");

    fs::remove_all(tmp);
    std::printf("[suite crypto] done\n");
}

// ---------------------------------------------------------------------------
// suite: pak（gpack + pak v4 加密往返，与引擎读写器互操作）
// ---------------------------------------------------------------------------
void run_pak() {
    using namespace grycegc;
    std::printf("[suite pak]\n");
    namespace fs = std::filesystem;
    namespace res = gryce_engine::resources;
    const fs::path tmp = temp_dir() / "grycegc_test_pak";
    fs::remove_all(tmp);
    mkdir_p(tmp);

    // 准备源文件
    const fs::path src = tmp / "src";
    mkdir_p(src / "models");
    mkdir_p(src / "scenes");
    const std::string c1 = "v 0.1 0.2 0.3\n";
    const std::string c2 = "scene gryce\n";
    const std::string c3 = "void main(){}\n";
    EXPECT(write_binary(src / "models" / "cube.obj", c1), "write obj");
    EXPECT(write_binary(src / "scenes" / "main.gesc", c2), "write gesc");
    EXPECT(write_binary(src / "simple.frag", c3), "write frag");

    Options o;
    o.positionals = {"create"};
    o.single["out"] = (tmp / "t.gpack").string();
    o.positionals.push_back(src.string());
    EXPECT(cmd_pak(o) == 0, "pak create gpack");

    // list / verify / extract
    Options lo;
    lo.positionals = {"list"};
    lo.single["file"] = (tmp / "t.gpack").string();
    EXPECT(cmd_pak(lo) == 0, "pak list gpack");

    Options vo;
    vo.positionals = {"verify"};
    vo.single["file"] = (tmp / "t.gpack").string();
    EXPECT(cmd_pak(vo) == 0, "pak verify gpack");

    Options eo;
    eo.positionals = {"extract"};
    eo.single["file"] = (tmp / "t.gpack").string();
    eo.single["out"] = (tmp / "out_gpack").string();
    EXPECT(cmd_pak(eo) == 0, "pak extract gpack");
    EXPECT(read_all(tmp / "out_gpack" / "models" / "cube.obj") ==
               std::vector<uint8_t>(c1.begin(), c1.end()),
           "extracted obj content");
    EXPECT(read_all(tmp / "out_gpack" / "scenes" / "main.gesc") ==
               std::vector<uint8_t>(c2.begin(), c2.end()),
           "extracted gesc content");

    // pak v4 加密往返
    uint8_t kb[32];
    for (int i = 0; i < 32; ++i) kb[i] = static_cast<uint8_t>(i * 5 + 3);
    const fs::path kf = tmp / "pakkey.bin";
    EXPECT(write_binary(kf, kb, sizeof(kb)), "write pak key");
    const std::string khex = [&] {
        static const char* d = "0123456789abcdef";
        std::string s;
        for (uint8_t b : kb) { s.push_back(d[b >> 4]); s.push_back(d[b & 0xF]); }
        return s;
    }();

    Options o2;
    o2.positionals = {"create"};
    o2.single["out"] = (tmp / "t.pak").string();
    o2.single["key"] = khex;
    o2.positionals.push_back(src.string());
    EXPECT(cmd_pak(o2) == 0, "pak create v4");

    Options v2;
    v2.positionals = {"verify"};
    v2.single["file"] = (tmp / "t.pak").string();
    v2.single["key"] = khex;
    EXPECT(cmd_pak(v2) == 0, "pak verify v4");

    Options e2;
    e2.positionals = {"extract"};
    e2.single["file"] = (tmp / "t.pak").string();
    e2.single["out"] = (tmp / "out_pak").string();
    e2.single["key"] = khex;
    EXPECT(cmd_pak(e2) == 0, "pak extract v4");
    EXPECT(read_all(tmp / "out_pak" / "simple.frag") ==
               std::vector<uint8_t>(c3.begin(), c3.end()),
           "extracted frag content (v4)");

    // 错误密钥：verify 必须失败
    Options v3;
    v3.positionals = {"verify"};
    v3.single["file"] = (tmp / "t.pak").string();
    v3.single["key"] = std::string(64, '0');
    EXPECT(cmd_pak(v3) != 0, "pak verify v4 wrong key fails");

    // 与引擎 PakReader 互操作（工具写的包引擎能读）
    {
        res::set_pak_crypto_key(std::string(reinterpret_cast<const char*>(kb), 32));
        res::PakReader pr;
        EXPECT(pr.open((tmp / "t.pak").string()), "engine PakReader opens grycegc pak");
        EXPECT(pr.contains("models/cube.obj"), "engine sees original path");
        auto data = pr.read("models/cube.obj");
        EXPECT(data.size() == c1.size() && std::memcmp(data.data(), c1.data(), c1.size()) == 0,
               "engine reads correct bytes");
        res::set_pak_crypto_key("");
    }

    std::error_code rec;
    fs::remove_all(tmp, rec);  // reader 句柄可能仍占用，忽略清理失败
    std::printf("[suite pak] done\n");
}

// ---------------------------------------------------------------------------
// suite: meta（gryce.meta.json / project.data / PE trailer）
// ---------------------------------------------------------------------------
void run_meta() {
    using namespace grycegc;
    std::printf("[suite meta]\n");
    namespace fs = std::filesystem;
    const fs::path tmp = temp_dir() / "grycegc_test_meta";
    fs::remove_all(tmp);
    mkdir_p(tmp);

    // 目录级 set/get/show
    Options s1;
    s1.positionals = {"set"};
    s1.single["key"] = "version";
    s1.single["value"] = "1.2.3";
    s1.single["target"] = tmp.string();
    EXPECT(cmd_meta(s1) == 0, "meta set dir");
    Options g1;
    g1.positionals = {"get"};
    g1.single["key"] = "version";
    g1.single["target"] = tmp.string();
    // get 打印到 stdout；直接验证文件内容
    auto j = meta_read_dir(tmp).to_json();
    EXPECT(j.contains("version") && j["version"] == "1.2.3", "meta json merged");
    EXPECT(cmd_meta(g1) == 0, "meta get dir");

    // stamp-git（仓库内应成功）
    Options st;
    st.positionals = {"stamp-git"};
    st.single["target"] = tmp.string();
    const int st_rc = cmd_meta(st);
    if (st_rc == 0) {
        auto j2 = meta_read_dir(tmp).to_json();
        EXPECT(j2.contains("build_id") && j2["build_id"].is_string() &&
                   !j2["build_id"].get<std::string>().empty(),
               "stamp-git wrote build_id");
        EXPECT(j2.contains("version") && j2["version"] == "1.2.3",
               "stamp-git preserved existing keys");
    } else {
        std::printf("  [skip] stamp-git（不在 git 仓库）\n");
    }

    // project.data 合并（保留既有键、覆盖同名键）
    EXPECT(write_binary(tmp / "project.data", "# comment\nname=demo\nrender_api=vulkan\n"),
           "write project.data");
    Options sp;
    sp.positionals = {"set"};
    sp.single["key"] = "render_api";
    sp.single["value"] = "opengl";
    sp.single["target"] = tmp.string();
    sp.flags.insert("project");
    EXPECT(cmd_meta(sp) == 0, "meta set project.data");
    EXPECT(project_data_get(tmp, "name") == "demo", "project.data keeps other keys");
    EXPECT(project_data_get(tmp, "render_api") == "opengl", "project.data overrides");
    Options sp2 = sp;
    sp2.single["key"] = "extra";
    sp2.single["value"] = "42";
    EXPECT(cmd_meta(sp2) == 0, "meta set project.data append");
    EXPECT(project_data_get(tmp, "extra") == "42", "project.data appends new key");

    // PE trailer 往返（拷贝 GryceGC.exe 自身当样本，避免动引擎产物）
    const fs::path self_exe = []() -> fs::path {
        char buf[MAX_PATH];
        if (GetModuleFileNameA(nullptr, buf, MAX_PATH) > 0) return fs::path(buf);
        return {};
    }();
    if (!self_exe.empty() && fs::exists(self_exe)) {
        const fs::path sample = tmp / "sample.exe";
        fs::copy_file(self_exe, sample, fs::copy_options::overwrite_existing);
        Options ts;
        ts.positionals = {"set"};
        ts.single["key"] = "build_id";
        ts.single["value"] = "deadbeef";
        ts.single["target"] = sample.string();
        EXPECT(cmd_meta(ts) == 0, "meta set pe trailer");
        auto tj = meta_read_pe_trailer(sample);
        EXPECT(tj.has_value() && tj->contains("build_id") &&
                   (*tj)["build_id"] == "deadbeef",
               "pe trailer roundtrip");
    } else {
        std::printf("  [skip] pe trailer（取不到自身路径）\n");
    }

    fs::remove_all(tmp);
    std::printf("[suite meta] done\n");
}

// ---------------------------------------------------------------------------
// suite: package（Packager 对真实构建树；不存在则跳过）
// ---------------------------------------------------------------------------
void run_package() {
    using namespace grycegc;
    std::printf("[suite package]\n");
    namespace fs = std::filesystem;
    std::vector<fs::path> candidates;
    // 1) CWD 相对路径
    candidates.emplace_back("build");
    candidates.emplace_back("cmake-build-debug");
    // 2) 从测试可执行文件反推引擎根 build（toolchain/build 或 build/toolchain，
    //    两种布局同深，../../build 均指向引擎根/build）
    char exe_path_buf[MAX_PATH];
    if (GetModuleFileNameA(nullptr, exe_path_buf, MAX_PATH) > 0) {
        const fs::path self(exe_path_buf);
        candidates.emplace_back(self.parent_path() / ".." / ".." / "build");
    }
    fs::path build_dir;
    for (const auto& c : candidates) {
        if (fs::is_directory(c) && fs::exists(c / "GryceRenderSmoke.exe")) {
            build_dir = fs::weakly_canonical(c);
            break;
        }
    }
    if (build_dir.empty()) {
        std::printf("  [skip] no engine build tree\n");
        std::printf("[suite package] done\n");
        return;
    }

    const fs::path out_root = temp_dir() / "grycegc_test_pkg";
    fs::remove_all(out_root);
    mkdir_p(out_root);

    PackageOptions po;
    po.build_dir = build_dir;
    po.output = out_root;
    po.name = "GryceEngine";
    // demo_project 相对仓库根（build_dir 的父目录），不依赖 CWD
    po.project_src = (build_dir.parent_path() / "test" / "demo_project").string();
    if (!fs::is_directory(po.project_src)) po.project_src.clear();
    Packager packager(po);
    const auto res = packager.run();
    EXPECT(res.ok, "package run");
    if (!res.ok) {
        std::printf("[suite package] done\n");
        return;
    }
    const fs::path pkg = res.pkg_root;
    EXPECT(fs::exists(pkg / "GryceRenderSmoke.exe"), "exe at pkg root");
    EXPECT(fs::exists(pkg / "GryceCore.dll"), "core dll at pkg root");
    EXPECT(fs::exists(pkg / "dll"), "dll/ folder exists");
    EXPECT(fs::exists(pkg / "run.bat"), "run.bat exists");
    EXPECT(fs::exists(pkg / "run_vulkan.bat"), "run_vulkan.bat exists");
    EXPECT(fs::exists(pkg / "run_play.bat"), "run_play.bat exists");
    EXPECT(fs::exists(pkg / "README.txt"), "README.txt exists");
    EXPECT(res.assets && fs::exists(pkg / "demo_project"), "demo_project packed");
    EXPECT(fs::exists(pkg / "dll" / "libstdc++-6.dll"), "mingw runtime in dll/");

    // 包自洽性：闭包全部可解析、无缺失
    DepResolver resolver(pkg, {pkg, pkg / "dll"});
    const auto closure = resolver.resolve(pkg / "GryceRenderSmoke.exe");
    EXPECT(closure.missing.empty(), "package closure has no missing dll");

    // 与 Python 版对拍已在 CLI 冒烟覆盖（字节级一致）；这里只验证二次打包稳定
    const auto res2 = packager.run();
    EXPECT(res2.ok, "package idempotent rerun");

    fs::remove_all(out_root);
    std::printf("[suite package] done\n");
}

// ---------------------------------------------------------------------------
// webpack：Emscripten 多文件产物 → 单文件自包含 HTML
// 用合成输入（不依赖真实构建树）覆盖全部路径与失败模式。
// ---------------------------------------------------------------------------

void run_webpack() {
    using namespace grycegc;
    std::printf("[suite webpack]\n");
    namespace fs = std::filesystem;

    const fs::path root = temp_dir() / "grycegc_test_webpack";
    fs::remove_all(root);
    mkdir_p(root);

    // 合成一个 Emscripten 风格的多文件产物（js 里故意放 </script> 验证转义）
    const fs::path in = root / "in";
    mkdir_p(in);
    const std::string html =
        "<!doctype html><html><head><title>Fake</title></head><body>"
        "<pre id=\"log\"></pre>"
        "<script>var Module={print:function(t){}};</script>"
        "<script async src=Fake.js></script>"
        "</body></html>";
    const std::string js =
        "var wasmBinaryFile=\"Fake.wasm\";var x=\"</script>\";// tail comment";
    const std::vector<uint8_t> wasm = {0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00};
    const std::vector<uint8_t> data = {'A', 'B', 'C', 'D'};  // base64: QUJDRA==
    auto write_vec = [](const fs::path& p, const std::vector<uint8_t>& v) {
        write_binary(p, std::string(v.begin(), v.end()));
    };
    write_binary(in / "Fake.html", html);
    write_binary(in / "Fake.js", js);
    write_vec(in / "Fake.wasm", wasm);
    write_vec(in / "Fake.data", data);

    const fs::path out = root / "out" / "Fake.single.html";

    // 1) 正常路径（显式指定全部输入）
    {
        WebPackOptions po;
        po.build_dir = in;
        po.name = "Fake";
        po.output = out;
        WebPacker packer(po);
        const auto res = packer.run();
        EXPECT(res.ok, res.error.empty() ? "webpack ok" : res.error.c_str());
        EXPECT(res.js_inlined, "js inlined flag");
        EXPECT(res.data_inlined, "data inlined flag");
        EXPECT(fs::is_regular_file(out), "output file exists");
        EXPECT(res.out_bytes > res.html_bytes + res.js_bytes, "output bigger than inputs");

        const auto o = read_all(out);
        const std::string s(o.begin(), o.end());
        EXPECT(s.find("data:application/wasm;base64,") != std::string::npos,
               "wasm data URL injected");
        EXPECT(s.find("QUJDRA==") != std::string::npos, "data base64 injected");
        EXPECT(s.find("M.getPreloadedPackage=function") != std::string::npos,
               "getPreloadedPackage hook injected");
        EXPECT(s.find("M.locateFile=function") != std::string::npos,
               "locateFile hook injected");
        EXPECT(s.find("src=Fake.js") == std::string::npos, "no external js reference");
        EXPECT(s.find("<\\/script>") != std::string::npos, "</script> escaped");
        EXPECT(s.find("</script>") != std::string::npos, "legit close tag kept");
        EXPECT(s.find("var Module={print:function(t){}};") != std::string::npos,
               "original shell script preserved");
        EXPECT(s.find("Fake.wasm") != std::string::npos, "wasm name in hook");
        EXPECT(s.find("Fake.data") != std::string::npos, "data name in hook");
    }

    // 2) 无 .data：只内联 wasm，不注入 getPreloadedPackage
    {
        const fs::path in2 = root / "in_nodata";
        mkdir_p(in2);
        write_binary(in2 / "Fake.html", html);
        write_binary(in2 / "Fake.js", js);
        write_vec(in2 / "Fake.wasm", wasm);
        const fs::path out2 = root / "out_nodata.html";

        WebPackOptions po;
        po.build_dir = in2;
        po.name = "Fake";
        po.output = out2;
        WebPacker packer(po);
        const auto res = packer.run();
        EXPECT(res.ok, "webpack without data ok");
        EXPECT(!res.data_inlined, "data_inlined false when .data missing");
        const auto o = read_all(out2);
        const std::string s(o.begin(), o.end());
        EXPECT(s.find("M.getPreloadedPackage=function") == std::string::npos,
               "no getPreloadedPackage hook without .data");
        EXPECT(s.find("data:application/wasm;base64,") != std::string::npos,
               "wasm still inlined");
    }

    // 3) 失败模式：缺 html
    {
        WebPackOptions po;
        po.build_dir = root / "nonexistent";
        po.name = "Fake";
        WebPacker packer(po);
        const auto res = packer.run();
        EXPECT(!res.ok, "missing html -> failure");
        EXPECT(!res.error.empty(), "error message set");
    }

    // 4) 失败模式：html 里没有 js script 标签
    {
        const fs::path in3 = root / "in_notag";
        mkdir_p(in3);
        write_binary(in3 / "Fake.html", "<html><body>no script here</body></html>");
        write_binary(in3 / "Fake.js", js);
        write_vec(in3 / "Fake.wasm", wasm);
        WebPackOptions po;
        po.build_dir = in3;
        po.name = "Fake";
        WebPacker packer(po);
        const auto res = packer.run();
        EXPECT(!res.ok, "html without js tag -> failure");
    }

    // 5) 单文件路径覆盖（--html/--js/--wasm/--data/--output 各自独立）
    {
        const fs::path in4 = root / "in_split";
        mkdir_p(in4);
        const fs::path other = root / "other";
        mkdir_p(other);
        write_binary(in4 / "entry.html",
                     "<!doctype html><html><body><script>var Module={};</script>"
                     "<script async src=bundle.js></script></body></html>");
        write_binary(other / "bundle.js", js);
        write_vec(other / "bundle.wasm", wasm);
        write_vec(other / "bundle.data", data);
        const fs::path out4 = root / "out_split.html";

        WebPackOptions po;
        po.html = in4 / "entry.html";  // 与其余输入分开放，验证逐项覆盖
        po.build_dir = in4;
        po.name = "bundle";
        po.js = other / "bundle.js";
        po.wasm = other / "bundle.wasm";
        po.data = other / "bundle.data";
        po.output = out4;
        WebPacker packer(po);
        const auto res = packer.run();
        EXPECT(res.ok, "split inputs ok");
        const auto o = read_all(out4);
        const std::string s(o.begin(), o.end());
        EXPECT(s.find("data:application/wasm;base64,") != std::string::npos,
               "split wasm inlined");
        EXPECT(s.find("src=bundle.js") == std::string::npos, "split js inlined");
    }

    fs::remove_all(root);
    std::printf("[suite webpack] done\n");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3 || std::strcmp(argv[1], "--suite") != 0) {
        std::fprintf(stderr, "usage: %s --suite <cli|crc32|pe|aes|smoke|crypto|pak|meta|package|webpack>\n",
                     argv[0]);
        return 2;
    }
    g_suite = argv[2];
    if (!std::strcmp(g_suite, "cli")) run_cli();
    else if (!std::strcmp(g_suite, "crc32")) run_crc32();
    else if (!std::strcmp(g_suite, "pe")) run_pe();
    else if (!std::strcmp(g_suite, "aes")) run_aes();
    else if (!std::strcmp(g_suite, "smoke")) run_smoke();
    else if (!std::strcmp(g_suite, "crypto")) run_crypto();
    else if (!std::strcmp(g_suite, "pak")) run_pak();
    else if (!std::strcmp(g_suite, "meta")) run_meta();
    else if (!std::strcmp(g_suite, "package")) run_package();
    else if (!std::strcmp(g_suite, "webpack")) run_webpack();
    else {
        std::fprintf(stderr, "unknown suite: %s\n", g_suite);
        return 2;
    }
    std::printf("[%s] %d/%d passed\n", g_suite, g_total - g_failures, g_total);
    return g_failures == 0 ? 0 : 1;
}
