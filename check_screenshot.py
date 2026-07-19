import struct, sys
path = sys.argv[1] if len(sys.argv) > 1 else 'screenshot_opengl.bmp'
with open(path, 'rb') as f:
    data = f.read()
header = data[:54]
width, height = struct.unpack('<ii', header[18:26])
offset = struct.unpack('<I', header[10:14])[0]
print(f'size={width}x{height}, offset={offset}, len={len(data)}')
bpp = struct.unpack('<H', header[28:30])[0]
bytes_per_pixel = bpp // 8
stride = ((width * bytes_per_pixel + 3) // 4) * 4
print(f'bpp={bpp}, stride={stride}')
pixels = []
for y in range(height):
    row_start = offset + (height - 1 - y) * stride
    for x in range(width):
        sample = data[row_start + x*bytes_per_pixel: row_start + x*bytes_per_pixel + bytes_per_pixel]
        if bytes_per_pixel == 4:
            b, g, r, a = sample
        else:
            b, g, r = sample
        pixels.append((r, g, b))
avg = tuple(sum(c[i] for c in pixels) // len(pixels) for i in range(3))
print('avg color (RGB):', avg)
# center 400x300 region
cx, cy = width // 2, height // 2
w2, h2 = 200, 150
center_pixels = [pixels[y*width+x] for y in range(cy-h2, cy+h2) for x in range(cx-w2, cx+w2)]
center_avg = tuple(sum(c[i] for c in center_pixels) // len(center_pixels) for i in range(3))
print('center avg color (RGB):', center_avg)
