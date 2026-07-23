import struct
import sys

src = sys.argv[1] if len(sys.argv) > 1 else 'screenshot_opengl.bmp'
dst = sys.argv[2] if len(sys.argv) > 2 else 'screenshot_cropped.bmp'
# Crop top-left 300x40 region (File menu area)
x, y, w, h = 0, 0, 300, 40

with open(src, 'rb') as f:
    data = f.read()

header = data[:54]
src_width, src_height = struct.unpack('<ii', header[18:26])
offset = struct.unpack('<I', header[10:14])[0]
bpp = struct.unpack('<H', header[28:30])[0]
bytes_per_pixel = bpp // 8
src_stride = ((src_width * bytes_per_pixel + 3) // 4) * 4
dst_stride = ((w * bytes_per_pixel + 3) // 4) * 4

# Write BMP header for cropped image
new_header = bytearray(header)
struct.pack_into('<i', new_header, 18, w)
struct.pack_into('<i', new_header, 22, h)
struct.pack_into('<I', new_header, 34, dst_stride * h)
struct.pack_into('<I', new_header, 10, 54)  # offset stays 54

pixels = bytearray()
for row in range(y, y + h):
    src_y = src_height - 1 - row
    src_row_start = offset + src_y * src_stride
    pixels.extend(data[src_row_start + x * bytes_per_pixel : src_row_start + (x + w) * bytes_per_pixel])
    # Pad to 4-byte boundary
    pad = dst_stride - w * bytes_per_pixel
    pixels.extend(b'\x00' * pad)

with open(dst, 'wb') as f:
    f.write(new_header)
    f.write(pixels)

print(f'Cropped {src} ({src_width}x{src_height}) -> {dst} ({w}x{h})')
