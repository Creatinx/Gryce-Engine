// ---------------------------------------------------------------------------
// GryceGC encrypt / decrypt / keygen（M3）
//
// 格式（容器）：
//   magic "GRYCENC1" + version u8(=1) + algo u8(1=aes-gcm, 2=chacha20)
//   + aad_len u16 + aad + payload
//   payload:
//     aes-gcm  : iv(12) || ct || tag(16)      —— 与引擎 aes_gcm_buf 字节兼容
//     chacha20 : nonce(12) || ct              —— 与引擎 pak_bundle v4 同款流
//   --raw：去掉容器头，仅 payload（aes-gcm 时可被引擎 aes_gcm_decrypt_buf 直接读）
//
// 密钥输入（--key）：
//   64 位十六进制串 / @文件（原样读前 32 字节）/ env:VAR / prompt:（控制台输入）
// 密钥绝不打印、绝不写入日志或元数据。
// ---------------------------------------------------------------------------

#include "../cli.h"
#include "../commands/commands.h"
#include "../core/fsutil.h"
#include "resources/aes_gcm.h"
#include "utils/crypto/chacha20.h"
#include "utils/crypto/random.h"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace grycegc {

namespace {

constexpr const char* kMagic = "GRYCENC1";
constexpr size_t kMagicLen = 8;
constexpr uint8_t kVersion = 1;
constexpr uint8_t kAlgoAesGcm = 1;
constexpr uint8_t kAlgoChaCha20 = 2;

// 从容器判断算法；不是本工具容器返回 0
uint8_t detect_algo(const std::vector<uint8_t>& data) {
    if (data.size() < kMagicLen + 2) return 0;
    if (std::memcmp(data.data(), kMagic, kMagicLen) != 0) return 0;
    return data[kMagicLen + 1];
}

bool hex_decode(const std::string& hex, std::vector<uint8_t>& out) {
    if (hex.size() % 2 != 0) return false;
    out.clear();
    out.reserve(hex.size() / 2);
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int hi = nib(hex[i]), lo = nib(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

std::string hex_encode(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s.push_back(d[p[i] >> 4]);
        s.push_back(d[p[i] & 0xF]);
    }
    return s;
}

// 解析 --key；失败返回空（错误信息经 err 返回）
bool parse_key(const std::string& spec, std::vector<uint8_t>& key, std::string& err) {
    key.clear();
    if (spec.rfind("@", 0) == 0) {
        auto bytes = read_all(spec.substr(1));
        if (bytes.empty()) { err = "密钥文件为空或不存在: " + spec.substr(1); return false; }
        if (bytes.size() < 32) { err = "密钥文件不足 32 字节"; return false; }
        key.assign(bytes.begin(), bytes.begin() + 32);
        return true;
    }
    if (spec.rfind("env:", 0) == 0) {
        const char* v = std::getenv(spec.substr(4).c_str());
        if (!v) { err = "环境变量未设置: " + spec.substr(4); return false; }
        return parse_key(v, key, err);
    }
    if (spec.rfind("prompt:", 0) == 0) {
        std::fprintf(stderr, "%s", spec.substr(7).c_str());
        std::string line;
        if (!std::getline(std::cin, line) || line.empty()) { err = "未输入密钥"; return false; }
        return parse_key(line, key, err);
    }
    // 默认按 hex 解析
    if (!hex_decode(spec, key) || key.size() != 32) {
        err = "密钥必须是 64 位十六进制（32 字节）";
        return false;
    }
    return true;
}

void put_u16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xFF));
    v.push_back(static_cast<uint8_t>(x >> 8));
}
uint16_t get_u16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

} // namespace

int cmd_keygen(const Options& o) {
    size_t bytes = 32;
    if (o.has("bytes")) bytes = static_cast<size_t>(o.get_int("bytes", 32));
    if (bytes == 0 || bytes > 4096) {
        std::fprintf(stderr, "[错误] --bytes 需在 1..4096（默认 32）\n");
        return 2;
    }
    std::vector<uint8_t> buf(bytes);
    if (!gryce_engine::utils::random_bytes(buf.data(), buf.size())) {
        std::fprintf(stderr, "[错误] 随机数生成失败\n");
        return 1;
    }
    const bool hex_out = o.is_flag("hex");
    const std::string out = hex_out ? hex_encode(buf.data(), buf.size())
                                    : std::string(reinterpret_cast<const char*>(buf.data()),
                                                  buf.size());
    if (o.has("out")) {
        if (!write_binary(o.get("out"), out)) {
            std::fprintf(stderr, "[错误] 写文件失败：%s\n", o.get("out").c_str());
            return 1;
        }
        std::fprintf(stderr, "[完成] 已写 %zu 字节密钥到 %s\n", bytes, o.get("out").c_str());
        return 0;
    }
    std::fwrite(out.data(), 1, out.size(), stdout);
    if (hex_out) std::fwrite("\n", 1, 1, stdout);
    return 0;
}

int cmd_encrypt(const Options& o) {
    if (!o.has("in") || !o.has("out")) {
        std::fprintf(stderr, "[错误] encrypt 需要 --in <file> --out <file>\n");
        return 2;
    }
    const std::string algo_name = o.get("algo", "aes-gcm");
    const uint8_t algo = (algo_name == "chacha20") ? kAlgoChaCha20 : kAlgoAesGcm;
    if (algo_name != "aes-gcm" && algo_name != "chacha20") {
        std::fprintf(stderr, "[错误] --algo 仅支持 aes-gcm / chacha20\n");
        return 2;
    }

    auto pt = read_all(o.get("in"));
    if (pt.empty()) {
        std::fprintf(stderr, "[错误] 输入文件为空或不存在：%s\n", o.get("in").c_str());
        return 2;
    }
    const std::string aad = o.get("aad", "");

    std::vector<uint8_t> key;
    std::string err;
    if (!o.has("key")) {
        std::fprintf(stderr, "[错误] 需要 --key（hex / @file / env:VAR / prompt:提示语）\n");
        return 2;
    }
    if (!parse_key(o.get("key"), key, err)) {
        std::fprintf(stderr, "[错误] %s\n", err.c_str());
        return 2;
    }

    std::vector<uint8_t> payload;
    if (algo == kAlgoAesGcm) {
        std::vector<uint8_t> enc;
        if (!gryce_engine::resources::aes_gcm_encrypt_buf(
                key.data(), reinterpret_cast<const uint8_t*>(aad.data()), aad.size(),
                pt.data(), pt.size(), enc)) {
            std::fprintf(stderr, "[错误] AES-GCM 加密失败\n");
            return 1;
        }
        payload = std::move(enc);
    } else {
        uint8_t nonce[12];
        if (!gryce_engine::utils::random_bytes(nonce, sizeof(nonce))) {
            std::fprintf(stderr, "[错误] 随机 nonce 生成失败\n");
            return 1;
        }
        payload.assign(pt.begin(), pt.end());
        gryce_engine::utils::chacha20_xor(key.data(), nonce, 0, payload.data(), payload.size());
        std::vector<uint8_t> with_nonce(nonce, nonce + 12);
        with_nonce.insert(with_nonce.end(), payload.begin(), payload.end());
        payload = std::move(with_nonce);
    }

    std::vector<uint8_t> out_bytes;
    if (!o.is_flag("raw")) {
        out_bytes.insert(out_bytes.end(), kMagic, kMagic + kMagicLen);
        out_bytes.push_back(kVersion);
        out_bytes.push_back(algo);
        put_u16(out_bytes, static_cast<uint16_t>(aad.size()));
        out_bytes.insert(out_bytes.end(), aad.begin(), aad.end());
    }
    out_bytes.insert(out_bytes.end(), payload.begin(), payload.end());

    if (!write_binary(o.get("out"), out_bytes.data(), out_bytes.size())) {
        std::fprintf(stderr, "[错误] 写输出失败：%s\n", o.get("out").c_str());
        return 1;
    }
    std::printf("[完成] %s → %s（%s，%zu 字节，%s）\n", o.get("in").c_str(),
                o.get("out").c_str(), algo_name.c_str(), out_bytes.size(),
                o.is_flag("raw") ? "raw（引擎兼容）" : "容器 GRYCENC1");
    return 0;
}

int cmd_decrypt(const Options& o) {
    if (!o.has("in") || !o.has("out")) {
        std::fprintf(stderr, "[错误] decrypt 需要 --in <file> --out <file>\n");
        return 2;
    }
    auto data = read_all(o.get("in"));
    if (data.empty()) {
        std::fprintf(stderr, "[错误] 输入文件为空或不存在：%s\n", o.get("in").c_str());
        return 2;
    }

    uint8_t algo = 0;
    std::string aad = o.get("aad", "");
    size_t payload_off = 0;

    const uint8_t detected = detect_algo(data);
    if (detected != 0) {
        algo = detected;
        if (algo == kAlgoAesGcm && o.has("algo") && o.get("algo") == "chacha20") {
            std::fprintf(stderr, "[错误] 文件是 aes-gcm 容器，与 --algo chacha20 矛盾\n");
            return 2;
        }
        if (algo == kAlgoChaCha20 && o.has("algo") && o.get("algo") == "aes-gcm") {
            std::fprintf(stderr, "[错误] 文件是 chacha20 容器，与 --algo aes-gcm 矛盾\n");
            return 2;
        }
        size_t off = kMagicLen + 2;
        const uint16_t aad_len = get_u16(data.data() + off);
        off += 2;
        if (off + aad_len > data.size()) {
            std::fprintf(stderr, "[错误] 容器头损坏（aad_len 越界）\n");
            return 2;
        }
        if (aad.empty())
            aad.assign(reinterpret_cast<const char*>(data.data() + off), aad_len);
        off += aad_len;
        payload_off = off;
    } else {
        // 无容器头：按 raw 处理，算法由 --algo 决定（默认 aes-gcm，兼容引擎格式）
        algo = (o.get("algo", "aes-gcm") == "chacha20") ? kAlgoChaCha20 : kAlgoAesGcm;
        payload_off = 0;
    }

    std::vector<uint8_t> key;
    std::string err;
    if (!o.has("key")) {
        std::fprintf(stderr, "[错误] 需要 --key\n");
        return 2;
    }
    if (!parse_key(o.get("key"), key, err)) {
        std::fprintf(stderr, "[错误] %s\n", err.c_str());
        return 2;
    }

    const uint8_t* payload = data.data() + payload_off;
    const size_t payload_len = data.size() - payload_off;

    std::vector<uint8_t> pt;
    if (algo == kAlgoAesGcm) {
        if (payload_len < 12 + 16) {
            std::fprintf(stderr, "[错误] aes-gcm payload 过短\n");
            return 2;
        }
        if (!gryce_engine::resources::aes_gcm_decrypt_buf(
                key.data(), reinterpret_cast<const uint8_t*>(aad.data()), aad.size(),
                payload, payload_len, pt)) {
            std::fprintf(stderr, "[错误] 解密失败（密钥错误 / 数据被篡改 / aad 不匹配）\n");
            return 1;
        }
    } else {
        if (payload_len < 12) {
            std::fprintf(stderr, "[错误] chacha20 payload 过短\n");
            return 2;
        }
        pt.assign(payload + 12, payload + payload_len);
        gryce_engine::utils::chacha20_xor(key.data(), payload, 0, pt.data(), pt.size());
    }

    if (!write_binary(o.get("out"), pt.data(), pt.size())) {
        std::fprintf(stderr, "[错误] 写输出失败：%s\n", o.get("out").c_str());
        return 1;
    }
    std::printf("[完成] %s → %s（%zu 字节）\n", o.get("in").c_str(), o.get("out").c_str(),
                pt.size());
    return 0;
}

} // namespace grycegc
