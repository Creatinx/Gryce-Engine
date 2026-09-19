#include "aes_gcm.h"

#include <cstdio>
#include <cstring>

#include "utils/glog/glog_lib.h"

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#endif

namespace gryce_engine::resources {

namespace {

// ============================================================================
// AES-256 块加密（Rijndael，Nk=8，Nr=14）
// ============================================================================

const uint8_t kSbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16};

const uint8_t kInvSbox[256] = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d};

const uint8_t kRcon[10] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36};

inline uint8_t xtime(uint8_t x) {
    return static_cast<uint8_t>((x << 1) ^ ((x & 0x80) ? 0x1B : 0));
}
inline uint8_t mul9(uint8_t x)  { return static_cast<uint8_t>(xtime(xtime(xtime(x))) ^ x); }
inline uint8_t mul11(uint8_t x) { return static_cast<uint8_t>(xtime(xtime(xtime(x))) ^ xtime(x) ^ x); }
inline uint8_t mul13(uint8_t x) { return static_cast<uint8_t>(xtime(xtime(xtime(x))) ^ xtime(xtime(x)) ^ x); }
inline uint8_t mul14(uint8_t x) { return static_cast<uint8_t>(xtime(xtime(xtime(x))) ^ xtime(xtime(x)) ^ xtime(x)); }

class Aes256 {
public:
    explicit Aes256(const uint8_t key[32]) {
        expand_key(key);
    }

    void encrypt_block(const uint8_t in[16], uint8_t out[16]) const {
        uint8_t state[16];
        std::memcpy(state, in, 16);
        add_round_key(state, 0);
        for (int round = 1; round <= 13; ++round) {
            sub_bytes(state);
            shift_rows(state);
            mix_columns(state);
            add_round_key(state, round);
        }
        sub_bytes(state);
        shift_rows(state);
        add_round_key(state, 14);
        std::memcpy(out, state, 16);
    }

    void decrypt_block(const uint8_t in[16], uint8_t out[16]) const {
        uint8_t state[16];
        std::memcpy(state, in, 16);
        add_round_key(state, 14);
        for (int round = 13; round >= 1; --round) {
            inv_shift_rows(state);
            inv_sub_bytes(state);
            add_round_key(state, round);
            inv_mix_columns(state);
        }
        inv_shift_rows(state);
        inv_sub_bytes(state);
        add_round_key(state, 0);
        std::memcpy(out, state, 16);
    }

private:
    void expand_key(const uint8_t key[32]) {
        uint8_t w[60][4];
        for (int i = 0; i < 8; ++i) {
            w[i][0] = key[4 * i];
            w[i][1] = key[4 * i + 1];
            w[i][2] = key[4 * i + 2];
            w[i][3] = key[4 * i + 3];
        }
        for (int i = 8; i < 60; ++i) {
            uint8_t t[4] = {w[i - 1][0], w[i - 1][1], w[i - 1][2], w[i - 1][3]};
            if (i % 8 == 0) {
                // RotWord
                const uint8_t tmp = t[0]; t[0] = t[1]; t[1] = t[2]; t[2] = t[3]; t[3] = tmp;
                // SubWord + Rcon
                t[0] = static_cast<uint8_t>(kSbox[t[0]] ^ kRcon[i / 8 - 1]);
                t[1] = kSbox[t[1]];
                t[2] = kSbox[t[2]];
                t[3] = kSbox[t[3]];
            } else if (i % 8 == 4) {
                t[0] = kSbox[t[0]];
                t[1] = kSbox[t[1]];
                t[2] = kSbox[t[2]];
                t[3] = kSbox[t[3]];
            }
            for (int j = 0; j < 4; ++j) {
                w[i][j] = static_cast<uint8_t>(w[i - 8][j] ^ t[j]);
            }
        }
        // round_key_[round*16 + 4*c + r] = w[round*4 + c][r]
        for (int round = 0; round < 15; ++round) {
            for (int c = 0; c < 4; ++c) {
                for (int r = 0; r < 4; ++r) {
                    round_key_[round * 16 + 4 * c + r] = w[round * 4 + c][r];
                }
            }
        }
    }

    void add_round_key(uint8_t state[16], int round) const {
        for (int i = 0; i < 16; ++i) {
            state[i] ^= round_key_[round * 16 + i];
        }
    }

    static void sub_bytes(uint8_t state[16]) {
        for (int i = 0; i < 16; ++i) state[i] = kSbox[state[i]];
    }

    static void inv_sub_bytes(uint8_t state[16]) {
        for (int i = 0; i < 16; ++i) state[i] = kInvSbox[state[i]];
    }

    static void shift_rows(uint8_t state[16]) {
        uint8_t t[16];
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                t[4 * c + r] = state[4 * ((c + r) % 4) + r];
            }
        }
        std::memcpy(state, t, 16);
    }

    static void inv_shift_rows(uint8_t state[16]) {
        uint8_t t[16];
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                t[4 * c + r] = state[4 * ((c + 4 - r) % 4) + r];
            }
        }
        std::memcpy(state, t, 16);
    }

    static void mix_columns(uint8_t state[16]) {
        for (int c = 0; c < 4; ++c) {
            uint8_t* s = state + 4 * c;
            const uint8_t a0 = s[0], a1 = s[1], a2 = s[2], a3 = s[3];
            s[0] = static_cast<uint8_t>(xtime(a0) ^ xtime3(a1) ^ a2 ^ a3);
            s[1] = static_cast<uint8_t>(a0 ^ xtime(a1) ^ xtime3(a2) ^ a3);
            s[2] = static_cast<uint8_t>(a0 ^ a1 ^ xtime(a2) ^ xtime3(a3));
            s[3] = static_cast<uint8_t>(xtime3(a0) ^ a1 ^ a2 ^ xtime(a3));
        }
    }

    static void inv_mix_columns(uint8_t state[16]) {
        for (int c = 0; c < 4; ++c) {
            uint8_t* s = state + 4 * c;
            const uint8_t a0 = s[0], a1 = s[1], a2 = s[2], a3 = s[3];
            s[0] = static_cast<uint8_t>(mul14(a0) ^ mul11(a1) ^ mul13(a2) ^ mul9(a3));
            s[1] = static_cast<uint8_t>(mul9(a0) ^ mul14(a1) ^ mul11(a2) ^ mul13(a3));
            s[2] = static_cast<uint8_t>(mul13(a0) ^ mul9(a1) ^ mul14(a2) ^ mul11(a3));
            s[3] = static_cast<uint8_t>(mul11(a0) ^ mul13(a1) ^ mul9(a2) ^ mul14(a3));
        }
    }

    static uint8_t xtime3(uint8_t x) {
        return static_cast<uint8_t>(xtime(x) ^ x);
    }

    uint8_t round_key_[240] = {};
};

// ============================================================================
// GCM 模式（GHASH 于 GF(2^128)）
// ============================================================================

// Z = X * Y（GF(2^128)，本原多项式 x^128 + x^7 + x^2 + x + 1）
void gf_mult(uint8_t z[16], const uint8_t x[16], const uint8_t y[16]) {
    uint8_t v[16];
    std::memcpy(v, x, 16);
    std::memset(z, 0, 16);
    for (int i = 0; i < 128; ++i) {
        if (y[i >> 3] & (0x80u >> (i & 7))) {
            for (int j = 0; j < 16; ++j) z[j] ^= v[j];
        }
        const uint8_t lsb = v[15] & 1;
        for (int j = 15; j >= 1; --j) {
            v[j] = static_cast<uint8_t>((v[j] >> 1) | ((v[j - 1] & 1) << 7));
        }
        v[0] >>= 1;
        if (lsb) v[0] ^= 0xE1;
    }
}

// S = GHASH(H, AAD || pad || C || pad || len64(AAD) || len64(C))
void ghash(const uint8_t h[16], const uint8_t* aad, size_t aad_len,
           const uint8_t* ct, size_t ct_len, uint8_t s[16]) {
    std::memset(s, 0, 16);

    auto process_blocks = [&](const uint8_t* data, size_t len) {
        size_t full = len / 16;
        for (size_t i = 0; i < full; ++i) {
            for (int j = 0; j < 16; ++j) s[j] ^= data[i * 16 + j];
            gf_mult(s, s, h);
        }
        size_t rem = len % 16;
        if (rem) {
            uint8_t block[16] = {};
            std::memcpy(block, data + full * 16, rem);
            for (int j = 0; j < 16; ++j) s[j] ^= block[j];
            gf_mult(s, s, h);
        }
    };

    process_blocks(aad, aad_len);
    process_blocks(ct, ct_len);

    uint8_t len_block[16] = {};
    const uint64_t aad_bits = static_cast<uint64_t>(aad_len) * 8;
    const uint64_t ct_bits = static_cast<uint64_t>(ct_len) * 8;
    for (int i = 0; i < 8; ++i) {
        len_block[i] = static_cast<uint8_t>(aad_bits >> (56 - 8 * i));
        len_block[8 + i] = static_cast<uint8_t>(ct_bits >> (56 - 8 * i));
    }
    for (int j = 0; j < 16; ++j) s[j] ^= len_block[j];
    gf_mult(s, s, h);
}

// 计数器自增（仅低 32 位，大端）
void inc32(uint8_t counter[16]) {
    for (int i = 15; i >= 12; --i) {
        if (++counter[i] != 0) break;
    }
}

// ============================================================================
// 随机 IV 生成
// ============================================================================

bool get_random_bytes(uint8_t* buf, size_t len) {
#if defined(_WIN32)
    return BCryptGenRandom(nullptr, buf, static_cast<ULONG>(len),
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    FILE* f = std::fopen("/dev/urandom", "rb");
    if (!f) return false;
    const size_t read = std::fread(buf, 1, len, f);
    std::fclose(f);
    return read == len;
#endif
}

} // namespace

// ============================================================================
// 对外接口
// ============================================================================

bool aes_gcm_encrypt(const uint8_t key[32], const uint8_t iv[12],
                     const uint8_t* aad, size_t aad_len,
                     const uint8_t* plaintext, size_t plaintext_len,
                     std::vector<uint8_t>& ciphertext, uint8_t tag[16]) {
    if (!key || !iv || (!plaintext && plaintext_len != 0)) return false;

    Aes256 aes(key);

    // H = E(K, 0^128)
    uint8_t h[16] = {};
    aes.encrypt_block(h, h);

    // J0 = IV || 0^31 || 1
    uint8_t j0[16] = {};
    std::memcpy(j0, iv, 12);
    j0[15] = 1;

    // CTR 加密（计数器从 J0+1 开始）
    uint8_t counter[16];
    std::memcpy(counter, j0, 16);
    inc32(counter);

    ciphertext.resize(plaintext_len);
    uint8_t ks[16];
    for (size_t i = 0; i < plaintext_len; i += 16) {
        aes.encrypt_block(counter, ks);
        const size_t n = (plaintext_len - i < 16) ? (plaintext_len - i) : 16;
        for (size_t j = 0; j < n; ++j) {
            ciphertext[i + j] = static_cast<uint8_t>(plaintext[i + j] ^ ks[j]);
        }
        inc32(counter);
    }

    // S = GHASH(H, AAD, C)
    uint8_t s[16];
    ghash(h, aad, aad_len, ciphertext.data(), plaintext_len, s);

    // T = E(K, J0) XOR S
    uint8_t e0[16];
    aes.encrypt_block(j0, e0);
    for (int i = 0; i < 16; ++i) {
        tag[i] = static_cast<uint8_t>(e0[i] ^ s[i]);
    }
    return true;
}

bool aes_gcm_decrypt(const uint8_t key[32], const uint8_t iv[12],
                     const uint8_t* aad, size_t aad_len,
                     const uint8_t* ciphertext, size_t ciphertext_len,
                     std::vector<uint8_t>& plaintext, const uint8_t tag[16]) {
    if (!key || !iv || (!ciphertext && ciphertext_len != 0) || !tag) return false;

    Aes256 aes(key);

    uint8_t h[16] = {};
    aes.encrypt_block(h, h);

    uint8_t j0[16] = {};
    std::memcpy(j0, iv, 12);
    j0[15] = 1;

    // CTR 解密
    uint8_t counter[16];
    std::memcpy(counter, j0, 16);
    inc32(counter);

    plaintext.resize(ciphertext_len);
    uint8_t ks[16];
    for (size_t i = 0; i < ciphertext_len; i += 16) {
        aes.encrypt_block(counter, ks);
        const size_t n = (ciphertext_len - i < 16) ? (ciphertext_len - i) : 16;
        for (size_t j = 0; j < n; ++j) {
            plaintext[i + j] = static_cast<uint8_t>(ciphertext[i + j] ^ ks[j]);
        }
        inc32(counter);
    }

    // 重算 tag 并恒定时间比较
    uint8_t s[16];
    ghash(h, aad, aad_len, ciphertext, ciphertext_len, s);

    uint8_t e0[16];
    aes.encrypt_block(j0, e0);

    uint8_t diff = 0;
    for (int i = 0; i < 16; ++i) {
        diff |= static_cast<uint8_t>(e0[i] ^ s[i] ^ tag[i]);
    }
    if (diff != 0) {
        plaintext.clear();
        return false;
    }
    return true;
}

bool aes_gcm_encrypt_buf(const uint8_t key[32],
                         const uint8_t* aad, size_t aad_len,
                         const uint8_t* plaintext, size_t plaintext_len,
                         std::vector<uint8_t>& out) {
    uint8_t iv[12];
    if (!get_random_bytes(iv, sizeof(iv))) {
        GLOG_ERROR("aes_gcm: failed to generate random IV");
        return false;
    }

    std::vector<uint8_t> ct;
    uint8_t tag[16];
    if (!aes_gcm_encrypt(key, iv, aad, aad_len, plaintext, plaintext_len, ct, tag)) {
        return false;
    }

    out.clear();
    out.reserve(12 + ct.size() + 16);
    out.insert(out.end(), iv, iv + 12);
    out.insert(out.end(), ct.begin(), ct.end());
    out.insert(out.end(), tag, tag + 16);
    return true;
}

bool aes_gcm_decrypt_buf(const uint8_t key[32],
                         const uint8_t* aad, size_t aad_len,
                         const uint8_t* data, size_t data_len,
                         std::vector<uint8_t>& plaintext) {
    if (!key || !data || data_len < 12 + 16) {
        plaintext.clear();
        return false;
    }
    const uint8_t* iv = data;
    const uint8_t* ct = data + 12;
    const size_t ct_len = data_len - 12 - 16;
    const uint8_t* tag = data + data_len - 16;
    return aes_gcm_decrypt(key, iv, aad, aad_len, ct, ct_len, plaintext, tag);
}

} // namespace gryce_engine::resources
