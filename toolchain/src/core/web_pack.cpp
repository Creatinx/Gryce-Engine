// ---------------------------------------------------------------------------
// GryceGC web_pack：Emscripten 多文件产物 → 单文件自包含 HTML
//
// 做法（链接后处理，不碰构建系统）：
//   1. 把 <script async src=NAME.js></script> 换成内联 <script>（转义 </script）
//   2. 在该 script 前注入一段 Module 补丁：
//        Module.locateFile      —— .wasm 返回 data: URL（fetch data: 不涉网络，
//                                  file:// 下 origin 'null' 的 CORS 管不着它）
//        Module.getPreloadedPackage —— .data 直接返回 ArrayBuffer，
//                                  file packager 的免 fetch 通道
//   两个钩子都是 Emscripten 运行时的公开 API（preamble.js 的 findWasmBinary /
//   locateFile，file_packager 生成码的 REMOTE_PACKAGE_NAME/getPreloadedPackage），
//   不依赖任何内部实现细节。
// ---------------------------------------------------------------------------

#include "core/web_pack.h"
#include "core/fsutil.h"

#include <cctype>
#include <cstdio>

namespace grycegc {
namespace {

const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const std::vector<uint8_t>& data) {
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 3 <= data.size(); i += 3) {
        const uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
        out.push_back(kB64[(v >> 18) & 63]);
        out.push_back(kB64[(v >> 12) & 63]);
        out.push_back(kB64[(v >> 6) & 63]);
        out.push_back(kB64[v & 63]);
    }
    if (i + 1 == data.size()) {
        const uint32_t v = uint32_t(data[i]) << 16;
        out.push_back(kB64[(v >> 18) & 63]);
        out.push_back(kB64[(v >> 12) & 63]);
        out.push_back('=');
        out.push_back('=');
    } else if (i + 2 == data.size()) {
        const uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
        out.push_back(kB64[(v >> 18) & 63]);
        out.push_back(kB64[(v >> 12) & 63]);
        out.push_back(kB64[(v >> 6) & 63]);
        out.push_back('=');
    }
    return out;
}

// 小写比较
std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// HTML 内联 JS 的安全转义：`</script` → `<\/script`（大小写不敏感）。
// `\` 对 `/` 的转义在 JS 字符串/正则/注释里都是恒等的，不会改变语义；
// 而 `</script>` 作为代码不可能出现，故转义永远安全。
std::string escape_inline_js(const std::string& js) {
    std::string out;
    out.reserve(js.size() + 64);
    const std::string lower_js = lower(js);
    size_t i = 0;
    while (i < js.size()) {
        if (lower_js.compare(i, 8, "</script") == 0) {
            out += "<\\/script";
            i += 8;
        } else {
            out.push_back(js[i]);
            ++i;
        }
    }
    return out;
}

// 在 html 里找引用指定 js 文件的 <script ...></script> 标签。
// Emscripten 输出形如 `<script async src=GryceRenderSmoke.js></script>`
// （属性无引号），也可能是 `<script src="x.js">`，两种都认。
// 返回 [begin, end)；找不到返回空 optional 语义（用 bool）。
bool find_js_script_tag(const std::string& html, const std::string& js_name,
                        size_t& begin, size_t& end) {
    size_t pos = 0;
    while ((pos = html.find("<script", pos)) != std::string::npos) {
        const size_t tag_end = html.find('>', pos);
        if (tag_end == std::string::npos) return false;
        const std::string tag = html.substr(pos, tag_end - pos + 1);
        // 标签里出现 src=<js_name>（带引号或不带）
        const std::string needle1 = "src=" + js_name;
        const std::string needle2 = "src=\"" + js_name + "\"";
        const std::string needle3 = "src='" + js_name + "'";
        if (tag.find(needle1) != std::string::npos || tag.find(needle2) != std::string::npos ||
            tag.find(needle3) != std::string::npos) {
            // 找配对的 </script>
            const size_t close = html.find("</script", tag_end);
            if (close == std::string::npos) return false;
            const size_t close_end = html.find('>', close);
            if (close_end == std::string::npos) return false;
            begin = pos;
            end = close_end + 1;
            return true;
        }
        pos = tag_end + 1;
    }
    return false;
}

} // namespace

void WebPacker::resolve_inputs(fs::path& html, fs::path& js, fs::path& wasm,
                               fs::path& data) const {
    html = opt_.html.empty() ? (opt_.build_dir / (opt_.name + ".html")) : opt_.html;
    js = opt_.js.empty() ? (opt_.build_dir / (opt_.name + ".js")) : opt_.js;
    wasm = opt_.wasm.empty() ? (opt_.build_dir / (opt_.name + ".wasm")) : opt_.wasm;
    data = opt_.data.empty() ? (opt_.build_dir / (opt_.name + ".data")) : opt_.data;
}

WebPackResult WebPacker::run() const {
    WebPackResult res;
    fs::path html_p, js_p, wasm_p, data_p;
    resolve_inputs(html_p, js_p, wasm_p, data_p);

    for (const fs::path* p : {&html_p, &js_p, &wasm_p}) {
        if (!fs::is_regular_file(*p)) {
            res.error = "找不到输入文件：" + p->string();
            return res;
        }
    }

    const std::string html = [&] {
        const auto v = read_all(html_p);
        return std::string(v.begin(), v.end());
    }();
    const auto js_v = read_all(js_p);
    const auto wasm_v = read_all(wasm_p);
    const std::string js(js_v.begin(), js_v.end());
    const bool has_data = fs::is_regular_file(data_p);
    std::vector<uint8_t> data_v;
    if (has_data) {
        data_v = read_all(data_p);
        if (data_v.empty()) {
            res.error = ".data 文件为空：" + data_p.string();
            return res;
        }
    }

    res.html_bytes = html.size();
    res.js_bytes = js.size();
    res.wasm_bytes = wasm_v.size();
    res.data_bytes = has_data ? data_v.size() : 0;

    // 1) 定位 js script 标签
    size_t tag_begin = 0, tag_end = 0;
    if (!find_js_script_tag(html, opt_.name + ".js", tag_begin, tag_end)) {
        res.error = "在 " + html_p.string() + " 里找不到引用 " + opt_.name +
                    ".js 的 <script> 标签（非 Emscripten html 产物？）";
        return res;
    }
    res.js_inlined = true;

    // 2) 组装 Module 补丁（插在 js script 之前，保证 Emscripten 顶层码执行时已就位）
    std::string patch;
    patch += "<script>\n";
    patch += "// [GryceGC webpack] 内联加载补丁（自动生成，请勿手改）。\n";
    patch += "// 把 .wasm 的运行时 fetch 换成 data: URL、.data 走 getPreloadedPackage\n";
    patch += "// 免 fetch 通道，使本页可从 file:// 直接打开（origin 'null' 的 CORS\n";
    patch += "// 会拦本地文件 XHR/fetch，但 data: URL 不涉网络，不受限制）。\n";
    patch += "(function(){\n";
    patch += "var M=window.Module=window.Module||{};\n";
    patch += "var prevLocate=M.locateFile;\n";
    patch += "M.locateFile=function(path,prefix){\n";
    patch += "if(path===\"" + opt_.name + ".wasm\")return \"data:application/wasm;base64," +
             base64_encode(wasm_v) + "\";\n";
    patch += "return prevLocate?prevLocate(path,prefix):(prefix||\"\")+path;\n";
    patch += "};\n";
    if (has_data) {
        patch += "M.getPreloadedPackage=function(name,size){\n";
        patch += "if(name===\"" + opt_.name + ".data\"){\n";
        patch += "var s=atob(\"" + base64_encode(data_v) + "\"),n=s.length,u=new Uint8Array(n);\n";
        patch += "for(var i=0;i<n;++i)u[i]=s.charCodeAt(i);\n";
        patch += "return u.buffer;\n";
        patch += "}\n";
        patch += "};\n";
        res.data_inlined = true;
    }
    patch += "})();\n";
    patch += "</script>\n";

    // 3) 内联 js + 插入补丁
    const std::string inline_js =
        "<script>" + escape_inline_js(js) + "</script>";
    std::string out = html.substr(0, tag_begin) + patch + inline_js +
                      html.substr(tag_end);

    // 4) 输出
    fs::path out_p = opt_.output.empty() ? (opt_.build_dir / (opt_.name + ".single.html"))
                                         : opt_.output;
    if (out_p.has_parent_path() && !out_p.parent_path().empty()) mkdir_p(out_p.parent_path());
    if (!write_binary(out_p, out)) {
        res.error = "写入失败：" + out_p.string();
        return res;
    }

    res.output = out_p;
    res.out_bytes = out.size();
    res.ok = true;
    return res;
}

} // namespace grycegc
