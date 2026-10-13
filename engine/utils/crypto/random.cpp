#include "random.h"

#include <cstdio>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#endif

namespace gryce_engine::utils {

#if defined(_WIN32)

bool random_bytes(uint8_t* buf, size_t len) {
    return BCryptGenRandom(nullptr, buf, static_cast<ULONG>(len),
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

#else

bool random_bytes(uint8_t* buf, size_t len) {
    FILE* f = std::fopen("/dev/urandom", "rb");
    if (!f) return false;
    const size_t read = std::fread(buf, 1, len, f);
    std::fclose(f);
    return read == len;
}

#endif

} // namespace gryce_engine::utils
