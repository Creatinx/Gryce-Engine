#!/usr/bin/env python
"""gen_imgui_font_data.py — 重新生成 engine/render/imgui_font_data.h。

用法（仓库根目录）：
    python tools/gen_imgui_font_data.py [源字体.ttf]

默认源：third_party/imgui/misc/fonts/Cousine-Regular.ttf（等宽，SIL OFL 1.1）。
输出：engine/render/imgui_font_data.h，内嵌为编译期字节数组，
      供 ImGuiRenderer 加载等宽字体（见 render/opengl/imgui_renderer.cpp）。
"""
import os
import sys

DEFAULT_SRC = "third_party/imgui/misc/fonts/Cousine-Regular.ttf"
OUT = "engine/render/imgui_font_data.h"
BYTES_PER_ROW = 12


def main() -> int:
    src = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_SRC
    if not os.path.isfile(src):
        print(f"font not found: {src}", file=sys.stderr)
        return 1
    data = open(src, "rb").read()
    name = os.path.basename(src)
    lines = [
        "#pragma once",
        "",
        "// ---------------------------------------------------------------------------",
        "// imgui_font_data.h — 引擎调试 UI 的内嵌等宽字体数据（自动生成，请勿手改）",
        "//",
        f"// 来源：{src}（{len(data):,} 字节）",
        f"// {name}：度量兼容 Courier New 的等宽字体，SIL Open Font License 1.1。",
        "// 以编译期数组内嵌，避免调试 UI 依赖运行时字体文件路径（工作目录无关）。",
        "// 生成方式：python tools/gen_imgui_font_data.py（如后续换字体，重新生成即可）。",
        "// ---------------------------------------------------------------------------",
        "",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "namespace gryce_engine::render {",
        "",
        "inline constexpr unsigned char kCousineRegularTtf[] = {",
    ]
    row = []
    for b in data:
        row.append("0x%02x," % b)
        if len(row) == BYTES_PER_ROW:
            lines.append("    " + "".join(row))
            row = []
    if row:
        lines.append("    " + "".join(row))
    lines += [
        "};",
        "",
        "inline constexpr size_t kCousineRegularTtfSize = sizeof(kCousineRegularTtf);",
        "",
        "} // namespace gryce_engine::render",
    ]
    with open(OUT, "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print(f"wrote {OUT} ({len(data):,} bytes from {src})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
