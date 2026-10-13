#!/usr/bin/env python3
"""找出两图差异最大的连通区域，并对每个区域给出两图各自的统计。"""
import sys

import numpy as np
from PIL import Image

ref_path, other_path = sys.argv[1], sys.argv[2]
flip = "--flip" in sys.argv
thr = 40
for i, a in enumerate(sys.argv):
    if a == "--threshold":
        thr = int(sys.argv[i + 1])

a = np.asarray(Image.open(ref_path).convert("RGB"), dtype=np.float32)
b = np.asarray(Image.open(other_path).convert("RGB"), dtype=np.float32)
if flip:
    b = b[::-1]

d = np.abs(a - b).max(axis=2)

# 先把差异图稀疏化（3x3 最大池化），再连通标记，避免碎块
h, w = d.shape
ds = d[: h // 3 * 3, : w // 3 * 3].reshape(h // 3, 3, w // 3, 3).max(axis=(1, 3))
mask = ds > thr

lab = np.zeros(mask.shape, dtype=np.int32)
cur = 0
stack = []
H, W = mask.shape
for y in range(H):
    for x in range(W):
        if mask[y, x] and lab[y, x] == 0:
            cur += 1
            lab[y, x] = cur
            stack.append((y, x))
            while stack:
                cy, cx = stack.pop()
                for ny, nx in ((cy-1, cx), (cy+1, cx), (cy, cx-1), (cy, cx+1)):
                    if 0 <= ny < H and 0 <= nx < W and mask[ny, nx] and lab[ny, nx] == 0:
                        lab[ny, nx] = cur
                        stack.append((ny, nx))

items = []
for k in range(1, cur + 1):
    ys, xs = np.nonzero(lab == k)
    if len(ys) < 100:
        continue
    y0, y1 = ys.min() * 3, min(h, (ys.max() + 1) * 3)
    x0, x1 = xs.min() * 3, min(w, (xs.max() + 1) * 3)
    items.append((len(ys) * 9, x0, x1, y0, y1))
items.sort(reverse=True)

print(f"{ref_path} vs {other_path}{' (other flipped)' if flip else ''}  阈值 |Δ|>{thr}")
print(f"差异连通块总数={cur}  有效(>100 格)={len(items)}")
for area, x0, x1, y0, y1 in items[:20]:
    ra = a[y0:y1, x0:x1]
    rb = b[y0:y1, x0:x1]
    rd = np.abs(ra - rb)
    print(f"  面积={area:8d} bbox x[{x0:4d},{x1:4d}] y[{y0:4d},{y1:4d}] "
          f"尺寸={x1-x0:4d}x{y1-y0:4d}")
    print(f"      参考 均值=({ra[...,0].mean():6.1f},{ra[...,1].mean():6.1f},{ra[...,2].mean():6.1f})  "
          f"对照 均值=({rb[...,0].mean():6.1f},{rb[...,1].mean():6.1f},{rb[...,2].mean():6.1f})")
    print(f"      区域平均|Δ|={rd.mean():.1f}  最大|Δ|={rd.max():.0f}  "
          f"超阈像素占比={(rd.max(axis=2) > thr).mean()*100:.1f}%")
