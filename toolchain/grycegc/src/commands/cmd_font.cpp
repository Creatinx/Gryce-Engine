// ---------------------------------------------------------------------------
// GryceGC font：重新生成 engine/render/imgui_font_data.h（M4）
//
// 移植自 tools/gen_imgui_font_data.py：
//   GryceGC font [源字体.ttf] [--out engine/render/imgui_font_data.h]
// 默认源：third_party/imgui/misc/fonts/Cousine-Regular.ttf（SIL OFL 1.1）。
// ---------------------------------------------------------------------------

#include "../cli.h"
#include "../commands/commands.h"
#include "../core/fsutil.h"

#include <cstdio>
#include <string>

namespace fs = std::filesystem;

namespace grycegc {

namespace {

constexpr size_t kBytesPerRow = 12;

// 千分位格式化（对齐 Python 的 f"{n:,}"）
std::string with_thousands(size_t n) {
    std::string s = std::to_string(n);
    std::string out;
    int count = 0;
    for (auto it = s.rbegin(); it != s.rend(); ++it) {
        if (count && count % 3 == 0) out.push_back(',');
        out.push_back(*it);
        ++count;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

} // namespace

int cmd_font(const Options& o) {
    fs::path src = o.has("src") ? o.get("src")
                                : fs::path("third_party/imgui/misc/fonts/Cousine-Regular.ttf");
    fs::path out = o.has("out") ? o.get("out") : fs::path("engine/render/imgui_font_data.h");
    if (!o.positionals.empty() && o.positionals.front() == "font" && o.positionals.size() > 1) {
        // 位置参数形式：GryceGC font <源.ttf>
        src = o.positionals[1];
    }

    auto data = read_all(src);
    if (data.empty()) {
        std::fprintf(stderr, "[错误] 字体文件不存在或为空：%s\n", src.string().c_str());
        return 1;
    }

    std::string s;
    s += "#pragma once\n\n";
    s += "// ---------------------------------------------------------------------------\n";
    s += "// imgui_font_data.h — 引擎调试 UI 的内嵌等宽字体数据（自动生成，请勿手改）\n";
    s += "//\n";
    s += "// 来源：" + src.string() + "（" + with_thousands(data.size()) + " 字节）\n";
    s += "// Cousine-Regular：度量兼容 Courier New 的等宽字体，SIL Open Font License 1.1。\n";
    s += "// 以编译期数组内嵌，避免调试 UI 依赖运行时字体文件路径（工作目录无关）。\n";
    s += "// 生成方式：GryceGC font（如后续换字体，重新生成即可）。\n";
    s += "// ---------------------------------------------------------------------------\n\n";
    s += "#include <cstddef>\n#include <cstdint>\n\n";
    s += "namespace gryce_engine::render {\n\n";
    s += "inline constexpr unsigned char kCousineRegularTtf[] = {\n";
    static const char* hexd = "0123456789abcdef";
    size_t col = 0;
    std::string row = "    ";
    for (uint8_t b : data) {
        row += "0x";
        row.push_back(hexd[b >> 4]);
        row.push_back(hexd[b & 0xF]);
        row += ",";
        if (++col == kBytesPerRow) {
            s += row + "\n";
            row = "    ";
            col = 0;
        }
    }
    if (col) s += row + "\n";
    s += "};\n\n";
    s += "inline constexpr size_t kCousineRegularTtfSize = sizeof(kCousineRegularTtf);\n\n";
    s += "} // namespace gryce_engine::render\n";

    if (!write_binary(out, s)) {
        std::fprintf(stderr, "[错误] 写失败：%s\n", out.string().c_str());
        return 1;
    }
    std::printf("wrote %s (%zu bytes from %s)\n", out.string().c_str(), data.size(),
                src.string().c_str());
    return 0;
}

} // namespace grycegc
