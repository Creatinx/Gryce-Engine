#pragma once

// ---------------------------------------------------------------------------
// GryceGC web_pack：把 Emscripten 多文件 Web 产物打成一个自包含 HTML
//
// 背景：Emscripten 默认产物是 html + js + wasm + data 四个文件，运行时用
// fetch/XHR 拉 wasm 与 .data。浏览器从 file:// 打开时 origin 为 'null'，
// CORS 会拦掉这些请求（Aborted(both async and sync fetching of the wasm
// failed)），必须起本地 HTTP 服务器才能跑。
//
// 本打包器做**链接后处理**（不碰构建系统）：把 js 内联进 html，并通过
// Emscripten 运行时的两个公开钩子消除剩余网络请求：
//   Module.locateFile(path, prefix)   —— findWasmBinary() 与 file packager
//                                        都经它定位文件名；对 .wasm 返回
//                                        data: URL（fetch data: 不涉网络）。
//   Module.getPreloadedPackage(n, s)  —— file packager 的免 fetch 通道，
//                                        直接返回 .data 的 ArrayBuffer。
// 产物是一个 html 文件，双击即用（与 UE5 的 Web 导出同形态），零外部引用。
//
// 体积代价：.wasm/.data 以 base64 内联（+33%）。
// ---------------------------------------------------------------------------

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace grycegc {

struct WebPackOptions {
    fs::path build_dir = "build-webgl";      // 产物所在目录
    std::string name = "GryceRenderSmoke";   // 应用基名（Emscripten 输出前缀）
    fs::path html;                           // 空 = <build_dir>/<name>.html
    fs::path js;                             // 空 = <build_dir>/<name>.js
    fs::path wasm;                           // 空 = <build_dir>/<name>.wasm
    fs::path data;                           // 空 = <build_dir>/<name>.data（可不存在）
    fs::path output;                         // 空 = <build_dir>/<name>.single.html
};

struct WebPackResult {
    bool ok = false;
    std::string error;
    fs::path output;
    size_t html_bytes = 0;    // 原始 html 大小
    size_t js_bytes = 0;      // 内联的 js 大小
    size_t wasm_bytes = 0;    // 内联的 wasm 大小（二进制）
    size_t data_bytes = 0;    // 内联的 .data 大小（0 = 无 data 内联）
    size_t out_bytes = 0;     // 产物大小
    bool js_inlined = false;  // 是否成功内联 js
    bool data_inlined = false;
};

class WebPacker {
public:
    explicit WebPacker(const WebPackOptions& opt) : opt_(opt) {}

    WebPackResult run() const;

private:
    // 定位四个输入文件（应用默认名规则），缺失的返回空路径
    void resolve_inputs(fs::path& html, fs::path& js, fs::path& wasm,
                        fs::path& data) const;

    WebPackOptions opt_;
};

} // namespace grycegc
