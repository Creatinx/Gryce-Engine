// ---------------------------------------------------------------------------
// GryceGC webpack：把 Emscripten 多文件 Web 产物打成一个自包含 HTML
//
// 用法: GryceGC webpack [options]
// 选项:
//   --build-dir <dir>   构建目录（默认 build-webgl）
//   --name <name>       应用基名 / Emscripten 输出前缀（默认 GryceRenderSmoke）
//   --html <file>       入口 html（默认 <build-dir>/<name>.html）
//   --js <file>         Emscripten JS（默认 <build-dir>/<name>.js）
//   --wasm <file>       wasm（默认 <build-dir>/<name>.wasm）
//   --data <file>       file packager 数据（默认 <build-dir>/<name>.data；
//                       文件不存在时自动跳过，只内联 wasm）
//   --output <file>     输出单文件 html（默认 <build-dir>/<name>.single.html）
//
// 背景：多文件产物运行时用 fetch 拉 .wasm/.data，file:// 直开会被 CORS 拦
// （origin 'null'）。本命令做链接后处理：内联 js + 经 Module.locateFile /
// Module.getPreloadedPackage 两个公开钩子消除网络请求，产物双击即用。
// 不修改构建系统，也不要求重新链接。
// ---------------------------------------------------------------------------

#include "../cli.h"
#include "../commands/commands.h"
#include "../core/web_pack.h"

#include <cstdio>

namespace fs = std::filesystem;

namespace grycegc {

int cmd_webpack(const Options& o) {
    WebPackOptions po;
    if (o.has("build-dir")) po.build_dir = o.get("build-dir");
    if (o.has("name")) po.name = o.get("name");
    if (o.has("html")) po.html = o.get("html");
    if (o.has("js")) po.js = o.get("js");
    if (o.has("wasm")) po.wasm = o.get("wasm");
    if (o.has("data")) po.data = o.get("data");
    if (o.has("output")) po.output = o.get("output");

    WebPacker packer(po);
    const auto res = packer.run();
    if (!res.ok) {
        std::fprintf(stderr, "[错误] %s\n", res.error.c_str());
        return 2;
    }

    std::printf("[完成] 单文件 Web 包已生成：%s\n", res.output.string().c_str());
    std::printf("  html 壳        : %zu B\n", res.html_bytes);
    std::printf("  js（已内联）   : %zu B\n", res.js_bytes);
    std::printf("  wasm（已内联） : %zu B\n", res.wasm_bytes);
    if (res.data_inlined) {
        std::printf("  data（已内联） : %zu B\n", res.data_bytes);
    } else {
        std::printf("  data           : 未找到，仅内联 wasm\n");
    }
    std::printf("  产物           : %zu B（%+.1f%% vs 多文件总和）\n", res.out_bytes,
                100.0 * (double(res.out_bytes) /
                             double(res.html_bytes + res.js_bytes + res.wasm_bytes +
                                    res.data_bytes) -
                         1.0));
    std::printf("  现在可以直接双击打开（file://），无需 HTTP 服务器。\n");
    return 0;
}

} // namespace grycegc
