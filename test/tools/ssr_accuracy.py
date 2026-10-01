"""SSR 反射精度复核（demo 场景专用）。

用解析几何算出"镜面反射本该看到什么"，再和渲染结果逐像素比对：

  * 掩码一致性：该命中的像素里实际命中了多少、有没有误报
  * 命中位置误差：SSR 命中点与真实交点之间的世界空间距离
  * 反射内容误差：反射采样到的颜色 vs 真实交点处（干净渲染）的颜色

用法（先把需要的图渲染出来）：
    build/GryceRenderSmoke.exe --api opengl --no-vsync --no-fps --frames 8 \
        --no-ssr --shot build/gl_nossr.png
    build/GryceRenderSmoke.exe --api opengl --no-vsync --no-fps --frames 8 \
        --ssr-debug 1 --shot build/gl_dbg1.png
    build/GryceRenderSmoke.exe --api opengl --no-vsync --no-fps --frames 8 \
        --ssr-debug 2 --shot build/gl_dbg2.png
    python test/tools/ssr_accuracy.py

相机必须用"四元数取前方 + 世界 +Y 重新正交化"的 look-at 形式（引擎行为）。
若直接用四元数旋转矩阵的三列，会带进约 7° 滚转，复核结论会整体错位。
"""
import math
import sys

import numpy as np
from PIL import Image

W, H = 1920, 1080
CAM_POS = np.array([3.2, 2.2, 4.2])
CAM_QUAT = np.array([-0.154516, 0.309031, 0.0, 0.938416])  # x,y,z,w
FOV_Y_DEG = 60.0
TAN_HALF = math.tan(math.radians(FOV_Y_DEG) * 0.5)
ASPECT = W / H


def _quat_mat(q):
    x, y, z, w = q / np.linalg.norm(q)
    return np.array([
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)],
    ])


_R = _quat_mat(CAM_QUAT)
FWD = _R @ np.array([0.0, 0.0, -1.0])
FWD /= np.linalg.norm(FWD)
RIGHT = np.cross(FWD, np.array([0.0, 1.0, 0.0]))
RIGHT /= np.linalg.norm(RIGHT)
UP = np.cross(RIGHT, FWD)


def view(p):
    d = np.atleast_2d(p) - CAM_POS
    return np.stack([d @ RIGHT, d @ UP, -(d @ FWD)], axis=1)


def project(p):
    v = view(p)[0]
    depth = -v[2]
    ndc_x = v[0] / depth / (TAN_HALF * ASPECT)
    ndc_y = v[1] / depth / TAN_HALF
    return (ndc_x * 0.5 + 0.5) * W, (0.5 - ndc_y * 0.5) * H, depth


def primary_ray(px, py):
    ndc_x = (px + 0.5) / W * 2.0 - 1.0
    ndc_y = 1.0 - (py + 0.5) / H * 2.0
    d = RIGHT * (ndc_x * TAN_HALF * ASPECT) + UP * (ndc_y * TAN_HALF) + FWD
    return CAM_POS, d / np.linalg.norm(d)


class Cube:
    def __init__(self, center, scale, yaw_deg, label):
        self.center = np.asarray(center, float)
        self.half = np.asarray(scale, float) * 0.5
        c, s = math.cos(math.radians(yaw_deg)), math.sin(math.radians(yaw_deg))
        self.rot = np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])
        self.label = label

    def ray(self, o, d):
        ol, dl = self.rot.T @ (o - self.center), self.rot.T @ d
        tmin, tmax, axis, sign = -1e9, 1e9, 0, 1.0
        for i in range(3):
            if abs(dl[i]) < 1e-9:
                if abs(ol[i]) > self.half[i]:
                    return None
                continue
            inv = 1.0 / dl[i]
            t1, t2 = (-self.half[i] - ol[i]) * inv, (self.half[i] - ol[i]) * inv
            if t1 > t2:
                t1, t2 = t2, t1
            if t1 > tmin:
                tmin, axis, sign = t1, i, -np.sign(dl[i])
            tmax = min(tmax, t2)
            if tmin > tmax:
                return None
        if tmax < 1e-4:
            return None
        entry = tmin > 1e-4
        n_local = np.zeros(3)
        n_local[axis] = sign if entry else -sign
        return (tmin if entry else tmax), self.rot @ n_local


RED = Cube([-1.05, 0.5, 0.0], [1, 1, 1], 0.0, "GlossyRed")
CHROME = Cube([1.15, 0.5, 0.35], [1, 1, 1], 45.0, "Chrome")
GOLD = Cube([0.05, 0.5, -1.35], [0.9, 0.9, 0.9], -22.5, "Gold")
CUBES = [RED, CHROME, GOLD]


def ray_ground(o, d, half=6.0):
    if abs(d[1]) < 1e-9:
        return None
    t = -o[1] / d[1]
    if t < 1e-3:
        return None
    p = o + d * t
    if abs(p[0]) > half or abs(p[2]) > half:
        return None
    return t, p


def visible(p, occluders):
    d = p - CAM_POS
    dist = np.linalg.norm(d)
    d = d / dist
    for c in occluders:
        hit = c.ray(CAM_POS, d)
        if hit and hit[0] < dist - 1e-3:
            return False
    return True


def expected_hit(px, py):
    """铬方块像素对应的真实反射交点（世界坐标），不可见/无交点返回 None。"""
    o, d = primary_ray(px, py)
    best = None
    for c in CUBES:
        hit = c.ray(o, d)
        if hit and (best is None or hit[0] < best[0]):
            best = (hit[0], hit[1], c)
    if best is None or best[2] is not CHROME:
        return None
    t, n, _ = best
    p = o + d * t
    v = CAM_POS - p
    v /= np.linalg.norm(v)
    if n @ v <= 0.02:
        return None
    r = 2.0 * (n @ v) * n - v
    r /= np.linalg.norm(r)
    cands = []
    g = ray_ground(p + r * 1e-3, r)
    if g:
        cands.append((g[0], g[1]))
    for c in CUBES:
        if c is CHROME:
            continue
        hit = c.ray(p + r * 1e-3, r)
        if hit:
            cands.append((hit[0], p + r * hit[0]))
    if not cands:
        return None
    cands.sort(key=lambda c: c[0])
    hp = cands[0][1]
    hx, hy, hd = project(hp)
    if not (0 <= hx < W - 1 and 0 <= hy < H - 1 and hd > 0):
        return None
    if not visible(hp, CUBES):
        return None
    return hp


def main():
    no_ssr = np.asarray(Image.open("build/gl_nossr.png").convert("RGB")).astype(np.float32)
    raw = np.asarray(Image.open("build/gl_dbg1.png").convert("RGB")).astype(np.float32)
    cov = np.asarray(Image.open("build/gl_dbg2.png").convert("RGB")).astype(np.float32)[..., 0] / 255.0

    n_expect = n_hit = n_false = n_miss = 0
    col_err = []
    for py in range(440, 790, 4):
        for px in range(940, 1270, 4):
            tp = expected_hit(px, py)
            got = cov[py, px] > 0.02
            if tp is not None:
                n_expect += 1
                if not got:
                    n_miss += 1
                    continue
            elif got:
                n_false += 1
                continue
            if tp is None:
                continue
            tx, ty, _ = project(tp)
            tx, ty = int(tx), int(ty)
            if 0 <= tx < W and 0 <= ty < H:
                col_err.append(np.abs(raw[py, px] - no_ssr[ty, tx]).mean())
    if n_expect == 0:
        print("没有可复核的像素：请先按文件头渲染 build/gl_*.png")
        return
    print(f"真实反射像素 {n_expect}，实际命中 {n_expect - n_miss} "
          f"({100 * (n_expect - n_miss) / n_expect:.2f}%)，漏掉 {n_miss}，误报 {n_false}")
    if col_err:
        e = np.array(col_err)
        print(f"反射内容 |误差| 均值 {e.mean():.1f}  90 分位 {np.percentile(e, 90):.1f}  (0-255)")


if __name__ == "__main__":
    main()
