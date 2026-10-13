#!/usr/bin/env python3
"""打印若干矩形区域的均值，用于多点取样对照。

用法: python crop_mean.py <img> x0 y0 x1 y1 [x0 y0 x1 y1 ...]
"""
import sys

import numpy as np
from PIL import Image

path = sys.argv[1]
nums = [int(v) for v in sys.argv[2:]]
a = np.asarray(Image.open(path).convert("RGB"), dtype=np.float32)
out = []
for i in range(0, len(nums), 4):
    x0, y0, x1, y1 = nums[i:i + 4]
    blk = a[y0:y1, x0:x1]
    out.append(f"[{x0},{y0}-{x1},{y1}] ({blk[...,0].mean():6.1f},{blk[...,1].mean():6.1f},"
               f"{blk[...,2].mean():6.1f})")
print(f"{path}: " + "  ".join(out))
