#!/usr/bin/env bash
# 无头 Chrome（SwiftShader 软件渲染 WebGL2）运行 Emscripten 产物并截图 + 读回 DOM。
#
# 用法：run_headless.sh <html文件> <截图输出.png> [窗口宽] [窗口高] [virtual-time-budget]
#
# 说明：
#   * --enable-unsafe-swiftshader：新版 Chrome 必须显式允许软件 WebGL；
#   * --virtual-time-budget：给 wasm 加载/编译/渲染留虚拟时间；
#   * --dump-dom：把页面 DOM 打到 stdout（shell 里 Module.print 收集的日志）。
set -u
HTML="$1"
SHOT="$2"
W="${3:-960}"
H="${4:-540}"
BUDGET="${5:-15000}"

CHROME="/c/Program Files/Google/Chrome/Application/chrome.exe"
if [ ! -f "$CHROME" ]; then
    CHROME="/c/Program Files (x86)/Microsoft/Edge/Application/msedge.exe"
fi

# URL 或本地路径（本地路径转 file:// URL）
case "$HTML" in
    http://*|https://*) URL="$HTML" ;;
    *) URL="file:///$(cygpath -w "$HTML" 2>/dev/null || echo "$HTML" | tr '/' '\\')" ;;
esac
SHOT_WIN=$(cygpath -w "$SHOT" 2>/dev/null || echo "$SHOT")

"$CHROME" --headless --no-sandbox --disable-gpu \
    --enable-unsafe-swiftshader \
    --screenshot="$SHOT_WIN" --window-size="${W},${H}" \
    --virtual-time-budget="$BUDGET" \
    --dump-dom \
    "$URL" 2>/dev/null
