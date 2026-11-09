"""生成 UI 演示贴图：一张照片 + 一组线性图标（纯标准库，可重复生成）。

输出：
  test/demo_project/ui/demo_photo.png   640x360 渐变风景（验证贴图缩放/裁切/颜色）
  test/demo_project/ui/icon_*.png       24x24 白色线性图标（按钮/菜单/命令栏用）
"""
import math
import os
import struct
import zlib

OUT = os.path.join(os.path.dirname(__file__), "..", "test", "demo_project", "ui")


def write_png(path, width, height, rgba):
    raw = bytearray()
    stride = width * 4
    for y in range(height):
        raw.append(0)
        raw.extend(rgba[y * stride:(y + 1) * stride])

    def chunk(tag, data):
        out = struct.pack(">I", len(data)) + tag + data
        return out + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def dist(px, py, ax, ay, bx, by):
    dx, dy = bx - ax, by - ay
    length2 = dx * dx + dy * dy
    if length2 <= 1e-9:
        return math.hypot(px - ax, py - ay)
    t = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / length2))
    return math.hypot(px - (ax + dx * t), py - (ay + dy * t))


def in_segment(px, py, ax, ay, bx, by, w):
    return dist(px, py, ax, ay, bx, by) <= w * 0.5


def in_ring(px, py, cx, cy, r, w):
    return abs(math.hypot(px - cx, py - cy) - r) <= w * 0.5


def in_disc(px, py, cx, cy, r):
    return math.hypot(px - cx, py - cy) <= r


def in_poly(px, py, pts):
    inside = False
    n = len(pts)
    for i in range(n):
        x0, y0 = pts[i]
        x1, y1 = pts[(i + 1) % n]
        if (y0 > py) != (y1 > py):
            xin = x0 + (py - y0) * (x1 - x0) / (y1 - y0)
            if px < xin:
                inside = not inside
    return inside


def in_arc(px, py, cx, cy, r, w, a0, a1):
    """角度区间（弧度，y 向下坐标系）内的圆弧。"""
    if abs(math.hypot(px - cx, py - cy) - r) > w * 0.5:
        return False
    ang = math.atan2(py - cy, px - cx)
    # 归一到 [a0, a0+2pi)
    while ang < a0:
        ang += math.pi * 2
    return ang <= a1


ICONS = {
    # 文件夹
    "folder": lambda x, y: (
        in_poly(x, y, [(4, 7), (10, 7), (11.5, 9), (20, 9), (20, 18.5), (4, 18.5)])
        and not in_poly(x, y, [(5.2, 10.2), (18.8, 10.2), (18.8, 17.3), (5.2, 17.3)])
    ),
    # 保存（方框 + 向下箭头，缩到 16px 也能认出来）
    "save": lambda x, y: (
        in_segment(x, y, 5, 5, 19, 5, 2)
        or in_segment(x, y, 19, 5, 19, 19, 2)
        or in_segment(x, y, 19, 19, 5, 19, 2)
        or in_segment(x, y, 5, 19, 5, 5, 2)
        or in_segment(x, y, 12, 7.5, 12, 13.5, 2)
        or in_segment(x, y, 8.8, 10.4, 12, 13.5, 2)
        or in_segment(x, y, 15.2, 10.4, 12, 13.5, 2)
    ),
    # 撤销（左向弧 + 箭头）
    "undo": lambda x, y: (
        in_arc(x, y, 12, 13, 6, 2, math.radians(170), math.radians(355))
        or in_segment(x, y, 6, 13, 6, 8, 2)
        or in_segment(x, y, 3.2, 10.4, 8.8, 10.4, 2)
    ),
    # 重做
    "redo": lambda x, y: (
        in_arc(x, y, 12, 13, 6, 2, math.radians(185), math.radians(370))
        or in_segment(x, y, 18, 13, 18, 8, 2)
        or in_segment(x, y, 15.2, 10.4, 20.8, 10.4, 2)
    ),
    # 播放
    "play": lambda x, y: in_poly(x, y, [(9, 5.5), (19, 12), (9, 18.5)]),
    # 搜索
    "search": lambda x, y: (
        in_ring(x, y, 10.5, 10.5, 6, 2) or in_segment(x, y, 15.2, 15.2, 20, 20, 2.2)
    ),
    # 设置（中心环 + 八根短粗齿）
    "settings": lambda x, y: (
        in_ring(x, y, 12, 12, 3.2, 2)
        or any(in_segment(x, y, 12 + 5.6 * math.cos(a), 12 + 5.6 * math.sin(a),
                          12 + 8.2 * math.cos(a), 12 + 8.2 * math.sin(a), 3.4)
               for a in [i * math.pi / 4 for i in range(8)])
    ),
    # 关闭
    "close": lambda x, y: (
        in_segment(x, y, 6.5, 6.5, 17.5, 17.5, 2.2)
        or in_segment(x, y, 17.5, 6.5, 6.5, 17.5, 2.2)
    ),
    # 对勾
    "check": lambda x, y: (
        in_segment(x, y, 5.5, 12.5, 9.5, 16.5, 2.2)
        or in_segment(x, y, 9.5, 16.5, 18.5, 7.0, 2.2)
    ),
}


def render_icon(shape, size=24, ss=4):
    data = bytearray(size * size * 4)
    for py in range(size):
        for px in range(size):
            hits = 0
            for sy in range(ss):
                for sx in range(ss):
                    x = px + (sx + 0.5) / ss
                    y = py + (sy + 0.5) / ss
                    if shape(x, y):
                        hits += 1
            cov = hits / (ss * ss)
            i = (py * size + px) * 4
            data[i] = 255
            data[i + 1] = 255
            data[i + 2] = 255
            data[i + 3] = int(cov * 255 + 0.5)
    return data


def render_photo(width=640, height=360):
    data = bytearray(width * height * 4)
    for y in range(height):
        v = y / (height - 1)
        for x in range(width):
            u = x / (width - 1)
            # 天空：顶部深蓝 -> 地平线暖橙
            r = int(28 + v * 150 + 60 * math.sin(u * 3.1) * (1 - v))
            g = int(42 + v * 90)
            b = int(88 + (1 - v) * 90)
            # 太阳
            sd = math.hypot((u - 0.72) * width, (v - 0.42) * height)
            glow = max(0.0, 1.0 - sd / 190.0) ** 2
            r = min(255, int(r + 210 * glow))
            g = min(255, int(g + 160 * glow))
            b = min(255, int(b + 60 * glow))
            # 远山剪影
            ridge = 0.62 + 0.08 * math.sin(u * 6.3) + 0.04 * math.sin(u * 17.0)
            if v > ridge:
                depth = (v - ridge)
                r = int(18 + 30 * depth)
                g = int(22 + 40 * depth)
                b = int(38 + 55 * depth)
            # 近处水面：太阳的竖向倒影 + 波纹扰动，避免规则横条
            if v > 0.78:
                refl = math.exp(-((u - 0.72) * 5.5) ** 2)
                ripple = 0.5 + 0.5 * math.sin(v * 90.0 + math.sin(u * 11.0) * 2.0)
                amount = refl * ripple * 150.0
                r = min(255, int(r + amount * 0.9))
                g = min(255, int(g + amount * 0.75))
                b = min(255, int(b + amount * 0.4))
            i = (y * width + x) * 4
            data[i] = max(0, min(255, r))
            data[i + 1] = max(0, min(255, g))
            data[i + 2] = max(0, min(255, b))
            data[i + 3] = 255
    return data


def main():
    os.makedirs(OUT, exist_ok=True)
    write_png(os.path.join(OUT, "demo_photo.png"), 640, 360, render_photo())
    for name, shape in ICONS.items():
        write_png(os.path.join(OUT, f"icon_{name}.png"), 24, 24, render_icon(shape))
    print("wrote", sorted(os.listdir(OUT)))


if __name__ == "__main__":
    main()
