"""渲染截图回归：驱动 GryceRenderSmoke 出图，校验画面不是清屏色，并可与本机基线比对。

纯标准库实现，不依赖 PIL。统计口径与 test/renderer_smoke.cpp 的 analyze_image
一致（每通道 5bit 直方图），阈值也对齐，这样脚本与宿主自身的判定不会互相矛盾。

用法：
  python tools/render_regression.py
  python tools/render_regression.py --update-baseline
  python tools/render_regression.py --baseline build/render_baseline.json
  python tools/render_regression.py --api vulkan --frames 120 --resolution 1024x768

默认不启用 CTest：无头环境、软件渲染与驱动差异会让像素统计漂移，CI 里跑它
既不稳定也不反映构建正确性（见 .github/workflows/ci.yml 的注释）。它是给
开发者本机做收口验收用的可复现命令。
"""

import argparse
import json
import struct
import subprocess
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MIN_NON_DOMINANT = 0.20
MIN_UNIQUE_BINS = 16
MEAN_TOLERANCE = 30.0
RATIO_TOLERANCE = 0.15


def decode_png(path):
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG: %s" % path)
    pos = 8
    width = height = 0
    bit_depth = color_type = 0
    idat = bytearray()
    while pos + 12 <= len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, bit_depth, color_type = struct.unpack(">IIBB", body[:10])
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
    if bit_depth != 8 or color_type not in (0, 2, 4, 6):
        raise ValueError("unsupported PNG layout: depth=%d type=%d" % (bit_depth, color_type))
    channels = {0: 1, 2: 3, 4: 2, 6: 4}[color_type]
    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    rows = []
    prev = bytearray(stride)
    off = 0
    for _ in range(height):
        filt = raw[off]
        off += 1
        line = bytearray(raw[off:off + stride])
        off += stride
        if filt == 1:
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif filt == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif filt == 3:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif filt == 4:
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                b = prev[i]
                c = prev[i - channels] if i >= channels else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 0xFF
        elif filt != 0:
            raise ValueError("unsupported PNG filter: %d" % filt)
        rows.append(line)
        prev = line
    return width, height, channels, b"".join(rows)


def image_stats(path):
    width, height, channels, pixels = decode_png(path)
    total = width * height
    histogram = {}
    sums = [0, 0, 0]
    for i in range(total):
        base = i * channels
        if channels == 1:
            r = g = b = pixels[base]
        elif channels == 2:
            r = g = b = pixels[base]
        else:
            r, g, b = pixels[base], pixels[base + 1], pixels[base + 2]
        sums[0] += r
        sums[1] += g
        sums[2] += b
        bin_id = ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)
        histogram[bin_id] = histogram.get(bin_id, 0) + 1
    dominant = max(histogram.values()) if histogram else total
    return {
        "width": width,
        "height": height,
        "mean": [round(v / total, 2) for v in sums],
        "non_dominant_ratio": round(1.0 - dominant / total, 4),
        "unique_bins": len(histogram),
    }


def default_exe():
    for name in ("GryceRenderSmoke.exe", "GryceRenderSmoke"):
        candidate = ROOT / "build" / name
        if candidate.exists():
            return str(candidate)
    return "GryceRenderSmoke.exe"


def main():
    ap = argparse.ArgumentParser(description="Gryce 渲染截图回归（本机验收，不进 CI）")
    ap.add_argument("--exe", default=default_exe(), help="GryceRenderSmoke 路径")
    ap.add_argument("--project", default=str(ROOT / "test" / "demo_project"), help="工程根目录")
    ap.add_argument("--api", default="vulkan", choices=("vulkan", "opengl", "dx12"))
    ap.add_argument("--frames", type=int, default=90)
    ap.add_argument("--resolution", default="800x600", help="视口 WxH")
    ap.add_argument("--out", default=str(ROOT / "build" / "render_regression.png"))
    ap.add_argument("--baseline", help="与基线 JSON 比对")
    ap.add_argument("--update-baseline", metavar="JSON", help="把本次统计写成基线")
    ap.add_argument("--ui-demo", action="store_true", help="同时注入 GryceUI 控件演示")
    ap.add_argument("--timeout", type=int, default=300)
    args = ap.parse_args()

    try:
        w, h = (int(v) for v in args.resolution.lower().split("x"))
    except ValueError:
        print("resolution 应为 WxH，例如 800x600", file=sys.stderr)
        return 1
    if w <= 0 or h <= 0 or args.frames <= 0:
        print("resolution / frames 必须为正数", file=sys.stderr)
        return 1

    out = Path(args.out).resolve()
    out.parent.mkdir(parents=True, exist_ok=True)
    if out.exists():
        out.unlink()

    cmd = [
        args.exe,
        "--api", args.api,
        "--project", args.project,
        "--frames", str(args.frames),
        "--shot", str(out),
        "--w", str(w),
        "--h", str(h),
        "--no-vsync",
        "--no-fps",
        "--no-debug-ui",
    ]
    if args.ui_demo:
        cmd.append("--ui-demo")
    print("run:", " ".join(cmd), flush=True)
    try:
        proc = subprocess.run(cmd, cwd=str(ROOT / "build"), timeout=args.timeout,
                              capture_output=True, text=True, errors="replace")
    except (OSError, subprocess.TimeoutExpired) as exc:
        print("启动截图失败: %s" % exc, file=sys.stderr)
        return 1
    tail = (proc.stdout or "").strip().splitlines()[-12:]
    for line in tail:
        print("  " + line)
    if proc.returncode != 0:
        print("宿主退出码 %d" % proc.returncode, file=sys.stderr)
        if proc.stderr:
            print(proc.stderr.strip()[-2000:], file=sys.stderr)
        return 1
    if not out.exists() or out.stat().st_size == 0:
        print("截图未生成: %s" % out, file=sys.stderr)
        return 1

    stats = image_stats(out)
    print("stats: %dx%d mean=%s non-dominant=%.2f%% bins=%d" % (
        stats["width"], stats["height"], stats["mean"],
        stats["non_dominant_ratio"] * 100.0, stats["unique_bins"]))

    failed = False
    if (stats["width"], stats["height"]) != (w, h):
        print("尺寸不符：期望 %dx%d，实际 %dx%d" % (
            w, h, stats["width"], stats["height"]), file=sys.stderr)
        failed = True
    if stats["non_dominant_ratio"] < MIN_NON_DOMINANT:
        print("画面接近纯清屏色（%.2f%% < %.2f%%），渲染链路可能未生效" % (
            stats["non_dominant_ratio"] * 100.0, MIN_NON_DOMINANT * 100.0),
            file=sys.stderr)
        failed = True
    if stats["unique_bins"] < MIN_UNIQUE_BINS:
        print("颜色档位只有 %d，画面缺少可辨识内容" % stats["unique_bins"], file=sys.stderr)
        failed = True
    if max(stats["mean"]) < 1.0:
        print("画面全黑", file=sys.stderr)
        failed = True

    if args.update_baseline:
        target = Path(args.update_baseline).resolve()
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(json.dumps(stats, indent=2, ensure_ascii=False) + "\n",
                          encoding="utf-8")
        print("baseline written: %s" % target)

    if args.baseline:
        base_path = Path(args.baseline).resolve()
        if not base_path.exists():
            print("基线不存在: %s" % base_path, file=sys.stderr)
            return 1
        base = json.loads(base_path.read_text(encoding="utf-8"))
        if (base.get("width"), base.get("height")) != (stats["width"], stats["height"]):
            print("基线与本次尺寸不一致", file=sys.stderr)
            failed = True
        for i in range(3):
            delta = abs(stats["mean"][i] - base["mean"][i])
            if delta > MEAN_TOLERANCE:
                print("通道 %d 均值偏离基线 %.1f（容差 %.1f）" % (
                    i, delta, MEAN_TOLERANCE), file=sys.stderr)
                failed = True
        delta = abs(stats["non_dominant_ratio"] - base["non_dominant_ratio"])
        if delta > RATIO_TOLERANCE:
            print("非主色占比偏离基线 %.3f（容差 %.3f）" % (
                delta, RATIO_TOLERANCE), file=sys.stderr)
            failed = True
        if not failed:
            print("baseline OK: %s" % base_path)

    if failed:
        print("render regression FAILED", file=sys.stderr)
        return 2
    print("render regression OK: %s" % out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
