"""按矩形裁剪 PNG（纯标准库），用于放大检查截图局部。

用法：python tools/png_crop.py <in.png> <out.png> <x> <y> <w> <h> [scale]
"""
import struct
import sys
import zlib


def read_png(path):
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a png"
    pos = 8
    width = height = 0
    bit_depth = color_type = 0
    idat = bytearray()
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, bit_depth, color_type = struct.unpack(">IIBB", body[:10])
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
    assert bit_depth == 8, "only 8-bit png"
    channels = {0: 1, 2: 3, 4: 2, 6: 4}[color_type]
    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    pixels = bytearray(width * height * channels)
    prev = bytearray(stride)
    off = 0
    for y in range(height):
        filt = raw[off]
        off += 1
        line = bytearray(raw[off:off + stride])
        off += stride
        if filt == 1:
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif filt == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif filt == 3:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif filt == 4:
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                b = prev[i]
                c = prev[i - channels] if i >= channels else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 0xFF
        pixels[y * stride:(y + 1) * stride] = line
        prev = line
    return width, height, channels, pixels


def write_png(path, width, height, channels, pixels):
    color_type = {1: 0, 3: 2, 4: 6}[channels]
    raw = bytearray()
    stride = width * channels
    for y in range(height):
        raw.append(0)
        raw += pixels[y * stride:(y + 1) * stride]

    def chunk(kind, body):
        return (struct.pack(">I", len(body)) + kind + body +
                struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF))

    out = b"\x89PNG\r\n\x1a\n"
    out += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, color_type, 0, 0, 0))
    out += chunk(b"IDAT", zlib.compress(bytes(raw), 6))
    out += chunk(b"IEND", b"")
    open(path, "wb").write(out)


def main():
    src, dst, x, y, w, h = sys.argv[1], sys.argv[2], *map(int, sys.argv[3:7])
    scale = int(sys.argv[7]) if len(sys.argv) > 7 else 1
    iw, ih, ch, px = read_png(src)
    x = max(0, min(x, iw - 1))
    y = max(0, min(y, ih - 1))
    w = max(1, min(w, iw - x))
    h = max(1, min(h, ih - y))
    ow, oh = w * scale, h * scale
    out = bytearray(ow * oh * ch)
    for oy in range(oh):
        sy = y + oy // scale
        for ox in range(ow):
            sx = x + ox // scale
            si = (sy * iw + sx) * ch
            di = (oy * ow + ox) * ch
            out[di:di + ch] = px[si:si + ch]
    write_png(dst, ow, oh, ch, out)
    print("cropped", dst, ow, oh)


if __name__ == "__main__":
    main()
