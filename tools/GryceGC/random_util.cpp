#include "random_util.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#endif

namespace gryce_engine::gc {

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

std::string random_base64_name() {
    uint8_t bytes[32];
    if (!random_bytes(bytes, sizeof(bytes))) {
        // 后备：进程级伪随机源，尽量填充 32 字节。
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

} // namespace gryce_engine::gc