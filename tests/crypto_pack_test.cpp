#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "resources/aes_gcm.h"
#include "resources/pak_bundle.h"
#include "resources/resource_loader.h"
#include "script/runtime/script_vm.h"

namespace fs = std::filesystem;
using namespace gryce_engine::resources;

// ============================================================================
// AES-256-GCM：NIST 标准向量验证块加密与 GCM 模式正确性
// ============================================================================

TEST(AesGcmTest, NistAes256GcmVector) {
    const uint8_t key[32] = {
        0xfe,0xff,0xe9,0x92,0x86,0x65,0x73,0x1c,0x6d,0x6a,0x8f,0x94,0x67,0x30,0x83,0x08,
        0xfe,0xff,0xe9,0x92,0x86,0x65,0x73,0x1c,0x6d,0x6a,0x8f,0x94,0x67,0x30,0x83,0x08};
    const uint8_t iv[12] = {0xca,0xfe,0xba,0xbe,0xfa,0xce,0xdb,0xad,0xde,0xca,0xf8,0x88};
    const uint8_t pt[64] = {
        0xd9,0x31,0x32,0x25,0xf8,0x84,0x06,0xe5,0xa5,0x59,0x09,0xc5,0xaf,0xf5,0x26,0x9a,
        0x86,0xa7,0xa9,0x53,0x15,0x34,0xf7,0xda,0x2e,0x4c,0x30,0x3d,0x8a,0x31,0x8a,0x72,
        0x1c,0x3c,0x0c,0x95,0x95,0x68,0x09,0x53,0x2f,0xcf,0x0e,0x24,0x49,0xa6,0xb5,0x25,
        0xb1,0x6a,0xed,0xf5,0xaa,0x0d,0xe6,0x57,0xba,0x63,0x7b,0x39,0x1a,0xaf,0xd2,0x55};
    const uint8_t expect_ct[64] = {
        0x52,0x2d,0xc1,0xf0,0x99,0x56,0x7d,0x07,0xf4,0x7f,0x37,0xa3,0x2a,0x84,0x42,0x7d,
        0x64,0x3a,0x8c,0xdc,0xbf,0xe5,0xc0,0xc9,0x75,0x98,0xa2,0xbd,0x25,0x55,0xd1,0xaa,
        0x8c,0xb0,0x8e,0x48,0x59,0x0d,0xbb,0x3d,0xa7,0xb0,0x8b,0x10,0x56,0x82,0x88,0x38,
        0xc5,0xf6,0x1e,0x63,0x93,0xba,0x7a,0x0a,0xbc,0xc9,0xf6,0x62,0x89,0x80,0x15,0xad};
    const uint8_t expect_tag[16] = {
        0xb0,0x94,0xda,0xc5,0xd9,0x34,0x71,0xbd,0xec,0x1a,0x50,0x22,0x70,0xe3,0xcc,0x6c};

    std::vector<uint8_t> ct;
    uint8_t tag[16] = {};
    ASSERT_TRUE(aes_gcm_encrypt(key, iv, nullptr, 0, pt, sizeof(pt), ct, tag));
    ASSERT_EQ(ct.size(), sizeof(pt));
    EXPECT_EQ(std::memcmp(ct.data(), expect_ct, sizeof(expect_ct)), 0);
    EXPECT_EQ(std::memcmp(tag, expect_tag, 16), 0);

    // 解密往返
    std::vector<uint8_t> rt;
    ASSERT_TRUE(aes_gcm_decrypt(key, iv, nullptr, 0, ct.data(), ct.size(), rt, tag));
    ASSERT_EQ(rt.size(), sizeof(pt));
    EXPECT_EQ(std::memcmp(rt.data(), pt, sizeof(pt)), 0);
}

TEST(AesGcmTest, NistEmptyPlaintext) {
    // NIST GCM AES-256 空明文向量
    const uint8_t key[32] = {};
    const uint8_t iv[12] = {};
    const uint8_t expect_tag[16] = {
        0x53,0x0f,0x8a,0xfb,0xc7,0x45,0x36,0xb9,0xa9,0x63,0xb4,0xf1,0xc4,0xcb,0x73,0x8b};

    std::vector<uint8_t> ct;
    uint8_t tag[16] = {};
    ASSERT_TRUE(aes_gcm_encrypt(key, iv, nullptr, 0, nullptr, 0, ct, tag));
    EXPECT_TRUE(ct.empty());
    EXPECT_EQ(std::memcmp(tag, expect_tag, 16), 0);
}

// ============================================================================
// AES-256-GCM：往返、篡改检测、AAD 绑定
// ============================================================================

TEST(AesGcmTest, RoundTripVariousLengths) {
    uint8_t key[32];
    resource_master_key(key);
    const uint8_t iv[12] = {0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c};
    std::vector<uint8_t> plain;
    for (size_t len : {0u, 1u, 15u, 16u, 17u, 31u, 64u, 255u, 1000u}) {
        plain.resize(len);
        for (size_t i = 0; i < len; ++i) plain[i] = static_cast<uint8_t>((i * 7 + 3) & 0xFF);

        std::vector<uint8_t> ct;
        uint8_t tag[16] = {};
        ASSERT_TRUE(aes_gcm_encrypt(key, iv, nullptr, 0, plain.data(), plain.size(), ct, tag));

        std::vector<uint8_t> rt;
        ASSERT_TRUE(aes_gcm_decrypt(key, iv, nullptr, 0, ct.data(), ct.size(), rt, tag));
        EXPECT_EQ(rt, plain) << "length " << len;
    }
}

TEST(AesGcmTest, TamperedCiphertextFailsTagCheck) {
    uint8_t key[32];
    resource_master_key(key);
    const std::string data = "sensitive payload that must be authenticated";

    std::vector<uint8_t> enc;
    ASSERT_TRUE(aes_gcm_encrypt_buf(key, nullptr, 0,
                                    reinterpret_cast<const uint8_t*>(data.data()),
                                    data.size(), enc));
    ASSERT_GT(enc.size(), 12u + 16u);

    // 篡改密文区一个字节
    std::vector<uint8_t> tampered = enc;
    tampered[20] ^= 0xFF;

    std::vector<uint8_t> out;
    EXPECT_FALSE(aes_gcm_decrypt_buf(key, nullptr, 0,
                                     tampered.data(), tampered.size(), out));
    EXPECT_TRUE(out.empty());

    // 篡改 tag 区一个字节
    std::vector<uint8_t> tampered_tag = enc;
    tampered_tag.back() ^= 0x01;
    EXPECT_FALSE(aes_gcm_decrypt_buf(key, nullptr, 0,
                                     tampered_tag.data(), tampered_tag.size(), out));

    // 错误密钥
    uint8_t other_key[32];
    resource_master_key(other_key);
    other_key[0] ^= 0x5A;
    EXPECT_FALSE(aes_gcm_decrypt_buf(other_key, nullptr, 0,
                                     enc.data(), enc.size(), out));
}

TEST(AesGcmTest, AadBinding) {
    uint8_t key[32];
    resource_master_key(key);
    const std::string data = "ui/main.uif payload";
    const std::string aad = "ui/main.uif";

    std::vector<uint8_t> enc;
    ASSERT_TRUE(aes_gcm_encrypt_buf(key, reinterpret_cast<const uint8_t*>(aad.data()), aad.size(),
                                    reinterpret_cast<const uint8_t*>(data.data()),
                                    data.size(), enc));

    std::vector<uint8_t> dec;
    ASSERT_TRUE(aes_gcm_decrypt_buf(key, reinterpret_cast<const uint8_t*>(aad.data()), aad.size(),
                                    enc.data(), enc.size(), dec));
    EXPECT_EQ(std::string(dec.begin(), dec.end()), data);

    // 错误 AAD 解密失败
    const std::string wrong_aad = "other";
    std::vector<uint8_t> bad;
    EXPECT_FALSE(aes_gcm_decrypt_buf(key, reinterpret_cast<const uint8_t*>(wrong_aad.data()),
                                     wrong_aad.size(), enc.data(), enc.size(), bad));
}

// ============================================================================
// JS 字节码：编译 -> 加载执行往返
// ============================================================================

TEST(CryptoPackTest, JsBytecodeRoundTrip) {
    GryceEngineUtils::script::ScriptVM vm;
    ASSERT_TRUE(vm.init());

    std::string err;
    std::vector<uint8_t> bc = vm.compile_script(
        "export function add(a, b) { return a + b; }\n"
        "export const version = '1.0';\n",
        "mod.js", true, &err);
    ASSERT_FALSE(bc.empty()) << err;

    JSValue module_ns = JS_UNDEFINED;
    auto er = vm.eval_bytecode(bc, "mod.js", &module_ns);
    ASSERT_TRUE(er.success) << er.error_msg;
    ASSERT_TRUE(JS_IsObject(module_ns));

    auto cr = vm.call_module_function(module_ns, "add",
                                      {GryceEngineUtils::script::JSValueWrapper(2),
                                       GryceEngineUtils::script::JSValueWrapper(3)});
    ASSERT_TRUE(cr.success) << cr.error_msg;
    EXPECT_EQ(cr.result, "5");

    // 非模块模式的全局字节码
    std::vector<uint8_t> global_bc = vm.compile_script(
        "globalThis.__gryce_test = 42;", "g.js", false, &err);
    ASSERT_FALSE(global_bc.empty()) << err;
    auto gr = vm.eval_bytecode(global_bc, "g.js");
    ASSERT_TRUE(gr.success) << gr.error_msg;
    EXPECT_EQ(vm.eval("__gryce_test").result, "42");

    JS_FreeValue(vm.context(), module_ns);
    vm.shutdown();
}

TEST(CryptoPackTest, JsBytecodeCompileError) {
    GryceEngineUtils::script::ScriptVM vm;
    ASSERT_TRUE(vm.init());

    std::string err;
    std::vector<uint8_t> bc = vm.compile_script("function ( {", "bad.js", false, &err);
    EXPECT_TRUE(bc.empty());
    EXPECT_FALSE(err.empty());
    vm.shutdown();
}

// ============================================================================
// ResourceLoader：发布包挂载、解密加载、篡改回退
// ============================================================================

TEST(CryptoPackTest, ResourceLoaderEndToEnd) {
    const fs::path dir = fs::temp_directory_path() / "gryce_crypto_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    // 1. 打包：JS 编译为字节码并加密，DSL 文本加密，其它资源原始
    GryceEngineUtils::script::ScriptVM vm;
    ASSERT_TRUE(vm.init());
    std::string err;
    std::vector<uint8_t> bc = vm.compile_script(
        "export function greet(name) { return 'hi ' + name; }", "scripts/ui_script.js", true, &err);
    ASSERT_FALSE(bc.empty()) << err;
    vm.shutdown();

    const std::string ui_text = "Window(id=\"main\", layout=\"vertical\") { Button(text=\"Go\") }";
    const std::vector<uint8_t> raw_bin = {'t','e','x','t',0x00,0x01,0x02,0xFF};

    PakWriter writer;
    ASSERT_TRUE(writer.add_buffer("scripts/ui_script.js", ResourceLoader::pack_js_bytecode(bc)));
    ASSERT_TRUE(writer.add_buffer("ui/main.uif", ResourceLoader::pack_ui_text(ui_text)));
    ASSERT_TRUE(writer.add_buffer("textures/icon.bin", ResourceLoader::pack_raw(raw_bin)));

    const fs::path pak = dir / "game.pak";
    ASSERT_TRUE(writer.write(pak.string()));

    // 2. 挂载并加载
    ResourceLoader loader;
    ASSERT_TRUE(loader.mount(pak.string()));
    EXPECT_EQ(loader.decrypt_ui_text("ui/main.uif"), ui_text);
    EXPECT_EQ(loader.read_raw("textures/icon.bin"), raw_bin);

    std::vector<uint8_t> loaded_bc = loader.decrypt_js_bytecode("scripts/ui_script.js");
    ASSERT_FALSE(loaded_bc.empty());

    GryceEngineUtils::script::ScriptVM vm2;
    ASSERT_TRUE(vm2.init());
    JSValue ns = JS_UNDEFINED;
    auto er = vm2.eval_bytecode(loaded_bc, "ui_script.js", &ns);
    ASSERT_TRUE(er.success) << er.error_msg;
    auto cr = vm2.call_module_function(ns, "greet", {GryceEngineUtils::script::JSValueWrapper("gryce")});
    ASSERT_TRUE(cr.success) << cr.error_msg;
    EXPECT_EQ(cr.result, "hi gryce");
    JS_FreeValue(vm2.context(), ns);
    vm2.shutdown();

    // 3. 篡改 .pak 中 JS 密文 -> 该块解密失败返回空（不崩溃），其它块不受影响
    {
        std::ifstream in(pak.string(), std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
        ASSERT_FALSE(bytes.empty());

        PakReader r;
        ASSERT_TRUE(r.open(pak.string()));
        const std::string random = r.resolve_random_path("scripts/ui_script.js");
        ASSERT_FALSE(random.empty());
        size_t off = 0;
        for (const auto& e : r.entries()) {
            if (e.path == random) {
                off = static_cast<size_t>(e.data_offset);
                break;
            }
        }
        ASSERT_GT(off, 0u);
        ASSERT_LT(off, bytes.size());
        bytes[off] ^= 0xFF;

        std::ofstream out(pak.string(), std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        out.close();
    }

    ResourceLoader loader2;
    ASSERT_TRUE(loader2.mount(pak.string()));
    EXPECT_TRUE(loader2.decrypt_js_bytecode("scripts/ui_script.js").empty());
    // DSL 块未被篡改，仍可正常解密（每块独立认证）
    EXPECT_EQ(loader2.decrypt_ui_text("ui/main.uif"), ui_text);

    fs::remove_all(dir, ec);
}
