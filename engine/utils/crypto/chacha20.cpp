#include "chacha20.h"

#include <cstring>

namespace gryce_engine::utils {

namespace {

inline uint32_t rotl32(uint32_t x, unsigned n) { return (x << n) | (x >> (32 - n)); }

void chacha20_quarter_round(uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d) {
    a += b; d = rotl32(d ^ a, 16);
    c += d; b = rotl32(b ^ c, 12);
    a += b; d = rotl32(d ^ a, 8);
    c += d; b = rotl32(b ^ c, 7);
}

// 生成单个 64 字节 keystream 块。counter 独立于 nonce（计数器低 32 位拼接 nonce）。
void chacha20_block(const uint8_t key[32], uint32_t counter,
                    const uint8_t nonce[12], uint8_t out[64]) {
    static const uint32_t kConst[4] = {0x61707865u, 0x3320646eu, 0x79622d32u, 0x6b206574u};
    uint32_t state[16];
    for (int i = 0; i < 4; ++i) state[i] = kConst[i];
    for (int i = 0; i < 8; ++i)
        state[4 + i] = (static_cast<uint32_t>(key[i * 4]) << 24) |
                       (static_cast<uint32_t>(key[i * 4 + 1]) << 16) |
                       (static_cast<uint32_t>(key[i * 4 + 2]) << 8) |
                       static_cast<uint32_t>(key[i * 4 + 3]);
    state[12] = counter;
    for (int i = 0; i < 3; ++i)
        state[13 + i] = (static_cast<uint32_t>(nonce[i * 4]) << 24) |
                        (static_cast<uint32_t>(nonce[i * 4 + 1]) << 16) |
                        (static_cast<uint32_t>(nonce[i * 4 + 2]) << 8) |
                        static_cast<uint32_t>(nonce[i * 4 + 3]);

    uint32_t ws[16];
    std::memcpy(ws, state, sizeof(ws));
    for (int i = 0; i < 10; ++i) {
        chacha20_quarter_round(ws[0], ws[4],  ws[8], ws[12]);
        chacha20_quarter_round(ws[1], ws[5],  ws[9], ws[13]);
        chacha20_quarter_round(ws[2], ws[6],  ws[10], ws[14]);
        chacha20_quarter_round(ws[3], ws[7],  ws[11], ws[15]);
        chacha20_quarter_round(ws[0], ws[5],  ws[10], ws[15]);
        chacha20_quarter_round(ws[1], ws[6],  ws[9], ws[12]);
        chacha20_quarter_round(ws[2], ws[7],  ws[10], ws[13]);
        chacha20_quarter_round(ws[3], ws[4],  ws[9], ws[14]);
    }
    for (int i = 0; i < 16; ++i) ws[i] += state[i];

    uint8_t* b = out;
    for (int i = 0; i < 16; ++i) {
        const uint32_t v = ws[i];
        b[i * 4]     = static_cast<uint8_t>(v);
        b[i * 4 + 1] = static_cast<uint8_t>(v >> 8);
        b[i * 4 + 2] = static_cast<uint8_t>(v >> 16);
        b[i * 4 + 3] = static_cast<uint8_t>(v >> 24);
    }
}

} // namespace

// 对 buf 的 len 个字节，用与 byte_offset 处 keystream 对齐的流做 XOR。
// 加密与解密都调用本函数（XOR 互逆）。
void chacha20_xor(const uint8_t key[32], const uint8_t nonce[12],
                  uint64_t byte_offset, uint8_t* buf, size_t len) {
    while (len > 0) {
        const uint64_t block_index = byte_offset / 64;
        const size_t start_in_block = static_cast<size_t>(byte_offset % 64);
        uint8_t ks[64];
        chacha20_block(key, static_cast<uint32_t>(block_index), nonce, ks);
        const size_t take = (len < (64 - start_in_block)) ? len : (64 - start_in_block);
        for (size_t i = 0; i < take; ++i) buf[i] ^= ks[start_in_block + i];
        buf += take;
        len -= take;
        byte_offset += take;
    }
}

} // namespace gryce_engine::utils
