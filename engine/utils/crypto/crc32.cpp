#include "crc32.h"

namespace gryce_engine::utils {

namespace {

uint32_t crc32_table[256];
bool crc32_table_initialized = []() {
    for (int i = 0; i < 256; ++i) {
        uint32_t c = static_cast<uint32_t>(i);
        for (int j = 0; j < 8; ++j) {
            c = (c >> 1) ^ (0xEDB88320u & static_cast<uint32_t>(-(c & 1u)));
        }
        crc32_table[i] = c;
    }
    return true;
}();

} // namespace

uint32_t crc32(const uint8_t* data, size_t len) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        c = crc32_table[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

} // namespace gryce_engine::utils
