"""GL / Vulkan 渲染一致性 + 屏幕空间效果自检。

用法（在仓库根目录执行）：

    python test/tools/backend_parity.py
    python test/tools/backend_parity.py --exe build/GryceRenderSmoke.exe --out build/parity

脚本会用同一个场景、同一份参数渲染 4 种组合（SSR × SSAO），然后给出：

1. 每个开关对画面的实际影响量（均值差 / 受影响像素占比）——用来发现
   "某个后端上这个效果完全不生效" 这类问题（历史上 Vulkan 的 SSAO、
   接触阴影都曾因为深度反投影错误而失效）。
2. GL 与 Vulkan 的逐像素差异（|Δ|>8 的占比）与反射贡献的相关性——用来断言
   "两个后端画面一致"。
3. 反射贡献（开 SSR − 关 SSR）的分布与包围盒——用来确认反射只作用在
   该反射的表面上（粗糙地面不该整片出现反射）。

依赖：numpy、Pillow。
"""

import argparse
import os
import subprocess
import sys

import numpy as np
from PIL import Image


def run_smoke(exe, api, out_path, extra):
    # --no-fps：FPS 叠加会画进截图（而且 GL 是字形、VK 目前是色块），
    # 会污染逐像素对比结果，测量时必须关掉。
    cmd = [exe, "--api", api, "--frames", "6", "--shot", out_path, "--no-analyze",
           "--no-fps", *extra]
    proc = subprocess.run(cmd, capture_output=True, text=True,
                          encoding="utf-8", errors="replace")
    if proc.returncode not in (0,):
        tail = "\n".join((proc.stdout or "").splitlines()[-15:])
        raise SystemExit(f"渲染失败: {' '.join(cmd)}\n{tail}")


def load(path):
    if not os.path.isfile(path):
        raise SystemExit(f"缺少截图: {path}")
    return np.asarray(Image.open(path).convert("RGB")).astype(np.float64)


def diff_stats(a, b):
    d = (a - b).mean(axis=2)
    return d.mean(), float((np.abs(d) > 8.0).mean() * 100.0), float(np.abs(d).max())


def corr(a, b):
    x = a.ravel() - a.mean()
    y = b.ravel() - b.mean()
    denom = np.linalg.norm(x) * np.linalg.norm(y)
    return float(x @ y / denom) if denom > 0 else float("nan")


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.abspath(os.path.join(here, "..", ".."))
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", default=os.path.join(root, "build", "GryceRenderSmoke.exe"))
    parser.add_argument("--out", default=os.path.join(root, "build", "parity"))
    args = parser.parse_args()

    os.makedirs(args.out, exist_ok=True)
    combos = {
        "gl_ssr_ssao": ("opengl", []),
        "gl_ssr": ("opengl", ["--no-ssao"]),
        "gl_none": ("opengl", ["--no-ssr", "--no-ssao"]),
        "gl_ssao": ("opengl", ["--no-ssr"]),
        "vk_ssr_ssao": ("vulkan", []),
        "vk_ssr": ("vulkan", ["--no-ssao"]),
        "vk_none": ("vulkan", ["--no-ssr", "--no-ssao"]),
        "vk_ssao": ("vulkan", ["--no-ssr"]),
    }
    shots = {}
    for name, (api, extra) in combos.items():
        path = os.path.join(args.out, name + ".png")
        print(f"[parity] 渲染 {name} ...", flush=True)
        run_smoke(args.exe, api, path, extra)
        shots[name] = load(path)

    print("\n== 开关效果（数值越大说明该效果越明显；两个后端应接近） ==")
    for api in ("gl", "vk"):
        # SSR 影响：SSR 开关（SSAO 关）；SSAO 影响：SSAO 开关（SSR 开）
        mean_ssr, ratio_ssr, _ = diff_stats(shots[f"{api}_ssr"], shots[f"{api}_none"])
        mean_ssao, ratio_ssao, _ = diff_stats(shots[f"{api}_ssr_ssao"], shots[f"{api}_ssr"])
        print(f"  {api.upper():3s}  SSR 影响: 均值 {mean_ssr:6.3f}  受影响像素 {ratio_ssr:5.2f}%"
              f"   |   SSAO 影响: 均值 {mean_ssao:6.3f}  受影响像素 {ratio_ssao:5.2f}%")

    print("\n== 后端一致性（GL vs Vulkan） ==")
    for label, a, b in (
        ("SSR+SSAO 全开", "gl_ssr_ssao", "vk_ssr_ssao"),
        ("仅 SSR", "gl_ssao", "vk_ssao"),
        ("全关", "gl_none", "vk_none"),
    ):
        mean_d, ratio, mx = diff_stats(shots[a], shots[b])
        verdict = "OK" if ratio < 0.5 else "不一致"
        print(f"  {label:14s} 均值差 {mean_d:+7.3f}  |Δ|>8 占比 {ratio:5.2f}%  最大 {mx:5.1f}  -> {verdict}")

    gl_refl = (shots["gl_ssr"] - shots["gl_none"]).mean(axis=2)
    vk_refl = (shots["vk_ssr"] - shots["vk_none"]).mean(axis=2)
    print(f"\n== 反射贡献一致性 ==\n  corr(GL, VK) = {corr(gl_refl, vk_refl):+.3f}")
    for tag, c in (("GL", gl_refl), ("VK", vk_refl)):
        ys, xs = np.nonzero(np.abs(c) > 1.0)
        box = (int(xs.min()), int(xs.max()), int(ys.min()), int(ys.max())) if ys.size else None
        print(f"  {tag}: 非零占比 {float((np.abs(c) > 1.0).mean()) * 100:5.2f}%  峰值 {c.max():6.2f}  bbox={box}")

    print("\n提示：反射只应出现在低粗糙度表面上；粗糙地面出现整片反射意味着法线/粗糙度缓冲或"
          "深度反投影出错。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
