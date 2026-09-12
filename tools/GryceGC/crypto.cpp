#include "crypto.h"

#include <cstring>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#else
#include <cstdint>
#endif

namespace gryce_engine::gc {

#if defined(_WIN32)
namespace {

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

} // namespace
#endif

#if !defined(_WIN32)
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

} // namespace gryce_engine::gc