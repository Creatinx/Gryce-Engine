/* stb_image_write.h - minimal PNG-only public domain writer
   This is a stripped-down version sufficient for Gryce Engine screenshots.
   Only stbi_write_png is implemented; other formats are intentionally omitted.
*/

#ifndef INCLUDE_STB_IMAGE_WRITE_H
#define INCLUDE_STB_IMAGE_WRITE_H

#ifndef STBI_WRITE_EXPORT
#define STBI_WRITE_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

STBI_WRITE_EXPORT int stbi_write_png(char const *filename, int w, int h, int comp, const void *data, int stride_in_bytes);

#ifdef __cplusplus
}
#endif

#endif // INCLUDE_STB_IMAGE_WRITE_H

#ifdef STB_IMAGE_WRITE_IMPLEMENTATION

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifndef STBIW_ASSERT
#include <cassert>
#define STBIW_ASSERT(x) assert(x)
#endif

// PNG chunk CRC32（多项式 0xEDB88320）。
// 表在首次调用时按标准算法生成：此前这里内嵌的表被截断成 192 项且后半段
// 有 108 项数值错误，越界读取导致每个 chunk 的 CRC 都是垃圾值——生成的
// PNG 能被宽松解码器"抢救"打开，但 PIL/浏览器等严格实现会直接判定为非
// 图片。改为运行时生成，杜绝抄表出错。
static const std::array<unsigned int, 256> &stbiw__crc_table() {
    static const std::array<unsigned int, 256> table = [] {
        std::array<unsigned int, 256> t{};
        for (unsigned int i = 0; i < 256; ++i) {
            unsigned int c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            t[i] = c;
        }
        return t;
    }();
    return table;
}

static unsigned int stbiw__crc32(const unsigned char *buffer, int len) {
    const std::array<unsigned int, 256> &crc_table = stbiw__crc_table();
    unsigned int crc = ~0u;
    for (int i = 0; i < len; ++i)
        crc = (crc >> 8) ^ crc_table[(buffer[i] ^ crc) & 0xff];
    return ~crc;
}

static unsigned int stbiw__adler32(const unsigned char *data, int len) {
    unsigned int s1 = 1, s2 = 0;
    for (int i = 0; i < len; ++i) {
        s1 = (s1 + data[i]) % 65521;
        s2 = (s2 + s1) % 65521;
    }
    return (s2 << 16) | s1;
}

static void stbiw__wp32(std::vector<unsigned char>& out, unsigned int v) {
    out.push_back(static_cast<unsigned char>(v >> 24));
    out.push_back(static_cast<unsigned char>(v >> 16));
    out.push_back(static_cast<unsigned char>(v >> 8));
    out.push_back(static_cast<unsigned char>(v));
}

static void stbiw__wpcrc(std::vector<unsigned char>& out, int len_at) {
    // PNG 的 chunk CRC 只覆盖 type + data（不含前面 4 字节的长度字段）。
    // len_at 指向长度字段起始处，故 CRC 从 len_at + 4（type 字段）开始，
    // 长度为 len_at 之后除长度字段外的全部字节。
    const int type_and_data_len = static_cast<int>(out.size()) - len_at - 4;
    unsigned int crc = stbiw__crc32(&out[len_at + 4], type_and_data_len);
    stbiw__wp32(out, crc);
}

STBI_WRITE_EXPORT int stbi_write_png(char const *filename, int w, int h, int comp, const void *data, int stride_in_bytes) {
    if (!filename || !data || w <= 0 || h <= 0 || comp != 4) return 0;
    if (stride_in_bytes == 0) stride_in_bytes = w * comp;

    FILE *f = nullptr;
#if defined(_MSC_VER) && _MSC_VER >= 1400
    fopen_s(&f, filename, "wb");
#else
    f = std::fopen(filename, "wb");
#endif
    if (!f) return 0;

    std::vector<unsigned char> out;
    // PNG signature
    const unsigned char sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    out.insert(out.end(), sig, sig + 8);

    // IHDR
    int ihdr_len_at = static_cast<int>(out.size());
    stbiw__wp32(out, 13);
    out.push_back('I'); out.push_back('H'); out.push_back('D'); out.push_back('R');
    stbiw__wp32(out, static_cast<unsigned int>(w));
    stbiw__wp32(out, static_cast<unsigned int>(h));
    out.push_back(8);  // bit depth
    out.push_back(6);  // color type RGBA
    out.push_back(0);  // compression
    out.push_back(0);  // filter
    out.push_back(0);  // interlace
    stbiw__wpcrc(out, ihdr_len_at);

    // Prepare filtered image data: each row is filter byte (0) + RGBA pixels
    std::vector<unsigned char> raw;
    raw.reserve(static_cast<size_t>(h) * (1 + static_cast<size_t>(w) * 4));
    const unsigned char *src = static_cast<const unsigned char *>(data);
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        const unsigned char *row = src + static_cast<size_t>(y) * stride_in_bytes;
        raw.insert(raw.end(), row, row + static_cast<size_t>(w) * 4);
    }

    // zlib stream: header + uncompressed deflate blocks + adler32
    std::vector<unsigned char> zlib;
    zlib.push_back(0x78);
    zlib.push_back(0x01);
    int pos = 0;
    while (pos < static_cast<int>(raw.size())) {
        int block = static_cast<int>(raw.size()) - pos;
        if (block > 32767) block = 32767;
        bool final = (pos + block >= static_cast<int>(raw.size()));
        zlib.push_back(final ? 0x01 : 0x00);
        zlib.push_back(static_cast<unsigned char>(block));
        zlib.push_back(static_cast<unsigned char>(block >> 8));
        zlib.push_back(static_cast<unsigned char>(~block));
        zlib.push_back(static_cast<unsigned char>(~block >> 8));
        zlib.insert(zlib.end(), raw.begin() + pos, raw.begin() + pos + block);
        pos += block;
    }
    unsigned int adler = stbiw__adler32(raw.data(), static_cast<int>(raw.size()));
    stbiw__wp32(zlib, adler);

    // IDAT
    int idat_len_at = static_cast<int>(out.size());
    stbiw__wp32(out, static_cast<unsigned int>(zlib.size()));
    out.push_back('I'); out.push_back('D'); out.push_back('A'); out.push_back('T');
    out.insert(out.end(), zlib.begin(), zlib.end());
    stbiw__wpcrc(out, idat_len_at);

    // IEND
    int iend_len_at = static_cast<int>(out.size());
    stbiw__wp32(out, 0);
    out.push_back('I'); out.push_back('E'); out.push_back('N'); out.push_back('D');
    stbiw__wpcrc(out, iend_len_at);

    std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
    return 1;
}

#endif // STB_IMAGE_WRITE_IMPLEMENTATION
