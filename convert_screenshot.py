from PIL import Image
import sys
src = sys.argv[1] if len(sys.argv) > 1 else 'D:/Gryce-Engine/screenshot_opengl.bmp'
dst = sys.argv[2] if len(sys.argv) > 2 else src.replace('.bmp', '.jpg')
img = Image.open(src)
img.save(dst, quality=90)
print(f'converted {src} ({img.size}) -> {dst}')
