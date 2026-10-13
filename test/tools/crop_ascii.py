#!/usr/bin/env python3
"""裁剪一块区域打印 ASCII 缩略图 + 数值统计。

用法: python crop_ascii.py <img> x0 x1 y0 y1 [cols rows]
"""
import sys

import numpy as np
from PIL import Image

path = sys.argv[1]
x0, x1, y0, y1 = (int(v) for v in sys.argv[2:6])
cols = int(sys.argv[6]) if len(sys.argv) > 6 else 60
rows = int(sys.argv[7]) if len(sys.argv) > 7 else 20

a = np.asarray(Image.open(path).convert("RGB"), dtype=np.float32)[y0:y1, x0:x1]
lum = 0.2126 * a[..., 0] + 0.7152 * a[..., 1] + 0.0722 * a[..., 2]
h, w = lum.shape
print(f"{path} crop x[{x0},{x1}) y[{y0},{y1}) 尺寸={w}x{h} "
      f"均值=({a[...,0].mean():.1f},{a[...,1].mean():.1f},{a[...,2].mean():.1f}) "
      f"min={lum.min():.0f} max={lum.max():.0f}")
ramp = " .:-=+*#%@"
for j in range(rows):
    line = ""
    for i in range(cols):
        blk = lum[j * h // rows:(j + 1) * h // rows, i * w // cols:(i + 1) * w // cols]
        line += ramp[min(9, int(blk.mean() / 255 * 9.999))]
    print(f"{j:2d} {line}")
