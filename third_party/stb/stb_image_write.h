/* stb_image_write.h - minimal PNG-only public domain writer
   This is a stripped-down version sufficient for Gryce Engine screenshots.
   Only stbi_write_png is implemented; other formats are intentionally omitted.
*/

#ifndef INCLUDE_STB_IMAGE_WRITE_H
#define INCLUDE_STB_IMAGE_WRITE_H

#ifdef __cplusplus
extern "C" {
#endif

int stbi_write_png(char const *filename, int w, int h, int comp, const void *data, int stride_in_bytes);

#ifdef __cplusplus
}
#endif

#endif // INCLUDE_STB_IMAGE_WRITE_H

#ifdef STB_IMAGE_WRITE_IMPLEMENTATION

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifndef STBIW_ASSERT
#include <cassert>
#define STBIW_ASSERT(x) assert(x)
#endif

static unsigned int stbiw__crc32(const unsigned char *buffer, int len) {
    static const unsigned int crc_table[256] = {
        0x00000000, 0x77073096, 0xEE0E612C, 0x990951BA, 0x076DC419, 0x706AF48F, 0xE963A535, 0x9E6495A3,
        0x0EDB8832, 0x79DCB8A4, 0xE0D5E91E, 0x97D2D988, 0x09B64C2B, 0x7EB17CBD, 0xE7B82D07, 0x90BF1D91,
        0x1DB71064, 0x6AB020F2, 0xF3B97148, 0x84BE41DE, 0x1ADAD47D, 0x6DDDE4EB, 0xF4D4B551, 0x83D385C7,
        0x136C9856, 0x646BA8C0, 0xFD62F97A, 0x8A65C9EC, 0x14015C4F, 0x63066CD9, 0xFA0F3D63, 0x8D080DF5,
        0x3B6E20C8, 0x4C69105E, 0xD56041E4, 0xA2677172, 0x3C03E4D1, 0x4B04D447, 0xD20D85FD, 0xA50AB56B,
        0x35B5A8FA, 0x42B2986C, 0xDBBBC9D6, 0xACBCF940, 0x32D86CE3, 0x45DF5C75, 0xDCD60DCF, 0xABD13D59,
        0x26D930AC, 0x51DE003A, 0xC8D75180, 0xBFD06116, 0x21B4F4B5, 0x56B3C423, 0xCFBA9599, 0xB8BDA50F,
        0x2802B89E, 0x5F058808, 0xC60CD9B2, 0xB10BE924, 0x2F6F7C87, 0x58684C11, 0xC1611DAB, 0xB6662D3D,
        0x76DC4190, 0x01DB7106, 0x98D220BC, 0xEFD5102A, 0x71B18589, 0x06B6B51F, 0x9FBFE4A5, 0xE8B8D433,
        0x7807C9A2, 0x0F00F934, 0x9609A88E, 0xE10E9818, 0x7F6A0DBB, 0x086D3D2D, 0x91646C97, 0xE6635C01,
        0x6B6B51F4, 0x1C6C6162, 0x856530D8, 0xF262004E, 0x1ADAD47D, 0x6DDDE4EB, 0xF4D4B551, 0x83D385C7,
        0x136C9856, 0x646BA8C0, 0xFD62F97A, 0x8A65C9EC, 0x14015C4F, 0x63066CD9, 0xFA0F3D63, 0x8D080DF5,
        0x3B6E20C8, 0x4C69105E, 0xD56041E4, 0xA2677172, 0x3C03E4D1, 0x4B04D447, 0xD20D85FD, 0xA50AB56B,
        0x35B5A8FA, 0x42B2986C, 0xDBBBC9D6, 0xACBCF940, 0x32D86CE3, 0x45DF5C75, 0xDCD60DCF, 0xABD13D59,
        0x26D930AC, 0x51DE003A, 0xC8D75180, 0xBFD06116, 0x21B4F4B5, 0x56B3C423, 0xCFBA9599, 0xB8BDA50F,
        0x2802B89E, 0x5F058808, 0xC60CD9B2, 0xB10BE924, 0x2F6F7C87, 0x58684C11, 0xC1611DAB, 0xB6662D3D,
        0x76DC4190, 0x01DB7106, 0x98D220BC, 0xEFD5102A, 0x71B18589, 0x06B6B51F, 0x9FBFE4A5, 0xE8B8D433,
        0x7807C9A2, 0x0F00F934, 0x9609A88E, 0xE10E9818, 0x7F6A0DBB, 0x086D3D2D, 0x91646C97, 0xE6635C01,
        0x6B6B51F4, 0x1C6C6162, 0x856530D8, 0xF262004E, 0x1ADAD47D, 0x6DDDE4EB, 0xF4D4B551, 0x83D385C7,
        0x136C9856, 0x646BA8C0, 0xFD62F97A, 0x8A65C9EC, 0x14015C4F, 0x63066CD9, 0xFA0F3D63, 0x8D080DF5,
        0x3B6E20C8, 0x4C69105E, 0xD56041E4, 0xA2677172, 0x3C03E4D1, 0x4B04D447, 0xD20D85FD, 0xA50AB56B,
        0x35B5A8FA, 0x42B2986C, 0xDBBBC9D6, 0xACBCF940, 0x32D86CE3, 0x45DF5C75, 0xDCD60DCF, 0xABD13D59,
        0x26D930AC, 0x51DE003A, 0xC8D75180, 0xBFD06116, 0x21B4F4B5, 0x56B3C423, 0xCFBA9599, 0xB8BDA50F,
        0x2802B89E, 0x5F058808, 0xC60CD9B2, 0xB10BE924, 0x2F6F7C87, 0x58684C11, 0xC1611DAB, 0xB6662D3D
    };
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
    int chunk_len = static_cast<int>(out.size()) - len_at - 4;
    unsigned int crc = stbiw__crc32(&out[len_at - 4], chunk_len + 4);
    stbiw__wp32(out, crc);
}

int stbi_write_png(char const *filename, int w, int h, int comp, const void *data, int stride_in_bytes) {
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
