#!/usr/bin/env python3
"""打印图像的唯一颜色数（快速判断"画面是否退化成平坦/量化"）。

用法: python unique_colors.py <图> [图...]
"""
import sys

import numpy as np
from PIL import Image

for path in sys.argv[1:]:
    a = np.asarray(Image.open(path).convert("RGB"), dtype=np.uint8).reshape(-1, 3)
    key = (a[:, 0].astype(np.uint32) << 16) | (a[:, 1].astype(np.uint32) << 8) | a[:, 2]
    print(f"{path}: unique_colors={len(np.unique(key))}")
