#!/usr/bin/env bash
# bench_ab.sh — Gryce-Engine Debug/Release 帧时间 A/B 基准
#
# 用法:
#   bash toolchain/bench_ab.sh <构建目录> <宽> <高> [次数]
#
# 打印每次运行的 CPU 帧耗时统计（平均/中位/p99）与分相均值、GPU 合计。
# 输出为单行 KV，便于 grep / 汇总。
set -u

DIR="${1:?用法: bench_ab.sh <build-dir> <w> <h> [runs]}"
W="${2:?缺宽带}"
H="${3:?缺高}"
RUNS="${4:-3}"

EXE="/d/Gryce-Engine/$DIR/GryceRenderSmoke.exe"
[ -x "$EXE" ] || { echo "ERR: 找不到 $EXE"; exit 1; }

cd "/d/Gryce-Engine/$DIR" || exit 1

for r in $(seq 1 "$RUNS"); do
    out="$(GLOG_minloglevel=2 ./GryceRenderSmoke.exe \
        --api opengl --w "$W" --h "$H" \
        --no-vsync --no-fps --no-analyze --bench 240 --shot _bench.png 2>&1)"

    # 注意：glog 会把形如 [2026/10/06/14/26/34] 的片段插进 stdout 中间，
    # 因此不能要求行首是 "[cpu]"；用子串匹配 + 去除插桩片段。
    out="$(printf '%s\n' "$out" | sed 's/\[[0-9][0-9]*\/[0-9][0-9]*\/[0-9][0-9]*\/[0-9][0-9]*\/[0-9][0-9]*\/[0-9][0-9]*\]//g')"

    cpu="$(printf '%s\n' "$out" | grep '帧耗时' | head -1)"
    pha="$(printf '%s\n' "$out" | grep '分相平均' | head -1)"
    gpu="$(printf '%s\n' "$out" | grep 'gpu\] 合计' | head -1)"

    avg="$(printf '%s' "$cpu" | sed -n 's/.*平均=\([0-9.]*\)ms.*/\1/p')"
    p50="$(printf '%s' "$cpu" | sed -n 's/.*中位=\([0-9.]*\)ms.*/\1/p')"
    p99="$(printf '%s' "$cpu" | sed -n 's/.*p99=\([0-9.]*\)ms.*/\1/p')"
    fps="$(printf '%s' "$cpu" | sed -n 's/.*(\([0-9.]*\) FPS).*/\1/p')"
    gtot="$(printf '%s' "$gpu" | sed -n 's/.*合计[[:space:]]*\([0-9.]*\).*/\1/p')"
    world="$(printf '%s' "$pha" | sed -n 's/.*world update=\([0-9.]*\)ms.*/\1/p')"
    rw="$(printf '%s' "$pha" | sed -n 's/.*RenderWorld=\([0-9.]*\)ms.*/\1/p')"
    ef="$(printf '%s' "$pha" | sed -n 's/.*EndFrame(提交+等GPU)=\([0-9.]*\)ms.*/\1/p')"

    printf 'RESULT dir=%-20s res=%sx%-5s run=%s avg=%sms fps=%s p50=%sms p99=%sms | gpu_total=%sms world=%sms RenderWorld=%sms EndFrame=%sms\n' \
        "$DIR" "$W" "$H" "$r" "$avg" "$fps" "$p50" "$p99" "$gtot" "$world" "$rw" "$ef"
done
