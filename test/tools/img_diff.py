#!/usr/bin/env python3
"""粗略对照两张截图：整体统计 + 亮度 ASCII 缩略图 + 分块差异热力图。

用法:
    python test/tools/img_diff.py <参考图> <对照图> [--blocks 48x27] [--art 120x34]

输出全部是文本，便于在无图形界面（或无法直接看图）的环境下判断「哪里不一样」。
"""
import argparse
import sys

import numpy as np
from PIL import Image


def load(path):
    im = Image.open(path).convert("RGB")
    return np.asarray(im, dtype=np.float32)


def luminance(rgb):
    return 0.2126 * rgb[..., 0] + 0.7152 * rgb[..., 1] + 0.0722 * rgb[..., 2]


def ascii_art(lum, cols, rows):
    """把亮度图降采样成 ASCII（' .:-=+*#%@'，暗→亮）。"""
    h, w = lum.shape
    ys = (np.arange(rows) * h // rows).clip(0, h - 1)
    xs = (np.arange(cols) * w // cols).clip(0, w - 1)
    # 用块内平均，比最近邻更能反映内容
    out = np.zeros((rows, cols), dtype=np.float32)
    y1 = ((np.arange(rows) + 1) * h // rows).clip(1, h)
    x1 = ((np.arange(cols) + 1) * w // cols).clip(1, w)
    for j in range(rows):
        for i in range(cols):
            out[j, i] = lum[ys[j]:y1[j], xs[i]:x1[i]].mean()
    ramp = " .:-=+*#%@"
    idx = np.clip((out / 255.0 * (len(ramp) - 1)).round().astype(int), 0, len(ramp) - 1)
    lines = []
    for j in range(rows):
        lines.append("".join(ramp[k] for k in idx[j]))
    return "\n".join(lines)


def block_map(diff, cols, rows, title):
    h, w = diff.shape
    y1 = ((np.arange(rows) + 1) * h // rows).clip(1, h)
    x1 = ((np.arange(cols) + 1) * w // cols).clip(1, w)
    print(f"--- {title} (每格 = 块内平均 |Δ| / 255) ---")
    header = "    " + "".join(str((i // 10) % 10) for i in range(cols))
    print(header)
    for j in range(rows):
        row = []
        for i in range(cols):
            v = diff[j * h // rows:y1[j], i * w // cols:x1[i]].mean()
            row.append(v)
        r = "".join(" .:-=+*#%@"[min(9, int(v * 255 / 26))] for v in row)
        print(f"{j:3d} {r}")
    # 打印每行的数值（便于量化）
    print("    每行平均: " + " ".join(
        f"{diff[j*h//rows:y1[j]].mean():.1f}" for j in range(rows)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ref")
    ap.add_argument("other")
    ap.add_argument("--blocks", default="48x27")
    ap.add_argument("--art", default="120x34")
    ap.add_argument("--flip", action="store_true",
                    help="先把对照图上下翻转再比（用于测试 Y 方向差异）")
    args = ap.parse_args()

    bc, br = (int(x) for x in args.blocks.split("x"))
    ac, ar = (int(x) for x in args.art.split("x"))

    a = load(args.ref)
    b = load(args.other)
    if args.flip:
        b = b[::-1]
    if a.shape != b.shape:
        print(f"尺寸不同: {a.shape} vs {b.shape}")
        sys.exit(1)

    la, lb = luminance(a), luminance(b)
    diff = np.abs(a - b).mean(axis=2)
    print(f"参考 {args.ref}: mean RGB=({a[...,0].mean():.1f},{a[...,1].mean():.1f},{a[...,2].mean():.1f})")
    print(f"对照 {args.other}: mean RGB=({b[...,0].mean():.1f},{b[...,1].mean():.1f},{b[...,2].mean():.1f})")
    print(f"像素平均 |Δ| = {diff.mean():.2f}/255   中位 = {np.median(diff):.2f}   "
          f"最大 = {diff.max():.0f}")
    for t in (2, 4, 8, 16, 32):
        print(f"  |Δ| > {t:2d} 的像素占比: {(diff > t).mean() * 100:6.2f}%")
    print(f"唯一颜色数: 参考={len(np.unique(a.reshape(-1,3), axis=0))}  "
          f"对照={len(np.unique(b.reshape(-1,3), axis=0))}")

    print()
    print(f"--- 参考 {args.ref} 亮度 ---")
    print(ascii_art(la, ac, ar))
    print()
    print(f"--- 对照 {args.other} 亮度 ---")
    print(ascii_art(lb, ac, ar))
    print()
    block_map(diff, bc, br, f"|Δ| {args.ref} vs {args.other}")


if __name__ == "__main__":
    main()
