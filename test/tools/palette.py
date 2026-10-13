#!/usr/bin/env python3
"""打印图像里出现频率最高的若干颜色（含占比），用于对比"色阶丰富度"。"""
import sys

import numpy as np
from PIL import Image

for path in sys.argv[1:]:
    a = np.asarray(Image.open(path).convert("RGB"), dtype=np.uint8)
    flat = a.reshape(-1, 3)
    key = (flat[:, 0].astype(np.uint32) << 16) | (flat[:, 1].astype(np.uint32) << 8) | flat[:, 2]
    uniq, counts = np.unique(key, return_counts=True)
    order = np.argsort(-counts)
    total = flat.shape[0]
    print(f"=== {path}  唯一色={len(uniq)} ===")
    for i in order[:12]:
        k = int(uniq[i])
        print(f"   ({k >> 16:3d},{k >> 8 & 255:3d},{k & 255:3d})  占比 {counts[i]/total*100:6.2f}%")
    # 出现次数 <= 200 的颜色（细节/抗锯齿/UI 文字）总量占比
    rare = counts[counts <= 500].sum()
    print(f"   低频色(<=500 像素)总数={len(uniq[counts <= 500])}  合计占比={rare/total*100:.2f}%")
