#include "registry.h"
#include "commands.h"

#include <algorithm>
#include <cstring>

namespace grycegc {

namespace {

const std::vector<Command>& kTable() {
    static const std::vector<Command> table = {
        {"version", "打印 GryceGC 版本",
         "用法: GryceGC version\n",
         &cmd_version},

        {"help", "显示帮助（无参数运行 GryceGC 等价于 help）",
         "用法: GryceGC help [command]\n"
         "无参数打印总览；带子命令名打印该命令详情。\n",
         &cmd_help},

        {"new", "创建项目骨架（main.js + project.data + 场景 + 立方体模型）",
         "用法: GryceGC new <dir> [options]\n"
         "选项:\n"
         "  --name <name>       项目显示名（默认取目录名）\n"
         "  --width <px>        默认窗口宽（默认 1280）\n"
         "  --height <px>       默认窗口高（默认 720）\n"
         "  --api <api>         默认渲染后端 opengl|vulkan|dx12（默认 opengl）\n"
         "  --extension         同时生成 extension.cpp 模板\n"
         "  --force             目标目录非空也继续（覆盖同名文件，保留其它文件）\n"
         "产物: main.js / project.data / scenes/main.gesc / models/cube.obj / README.md\n"
         "      项目不需要自己的 main.cpp；着色器由引擎内置目录兜底。\n",
         &cmd_new},

        {"deps", "列出可执行文件的完整 DLL 依赖闭包（按 core / 依赖分类）",
         "用法: GryceGC deps [options]\n"
         "选项:\n"
         "  --build-dir <dir>   CMake 构建目录（缺省自动探测：build → cmake-build-debug → cmake-build-release）\n"
         "  --exe <path>        可执行文件路径或名称（默认在 build-dir 内自动探测）\n"
         "  --extra-dir <dir>   额外 DLL 搜索目录（可重复）\n",
         &cmd_deps},

        {"verify", "校验发布包自洽性：所有导入 DLL 在包内可解析",
         "用法: GryceGC verify --package <dir> [--dll-folder <name>]\n"
         "选项:\n"
         "  --package <dir>     发布包目录（必填）\n"
         "  --dll-folder <name> 依赖 DLL 所在子目录名（默认 dll）\n",
         &cmd_verify},

        {"package", "打包成可脱线运行的发布目录（移植自 toolchain/main.py）",
         "用法: GryceGC package [options]\n"
         "选项:\n"
         "  --build-dir <dir>   CMake 构建目录（相对路径以引擎根为基；缺省自动探测 build → cmake-build-debug → cmake-build-release，优先含 GryceApp.exe 的）\n"
         "  --exe <path>        可执行文件（默认自动探测 GryceApp.exe）\n"
         "  --output <dir>      输出根目录（默认 dist）\n"
         "  --name <name>       发布包文件夹名（默认 GryceEngine）\n"
         "  --project <dir>     资源项目根（默认 test/demo_project；会被拷进包内 demo_project/）\n"
         "  --dll-folder <n>    依赖 DLL 文件夹名（默认 dll）\n"
         "  --api <api>         默认启动器后端 opengl|vulkan；缺省读 project.data 的 render_api\n"
         "  --extra-dir <dir>   额外 DLL 搜索目录（可重复）\n"
         "  --no-assets         不打包 demo_project 资源\n"
         "  --clean             打包前清空输出目录\n"
         "  --verify            打包后立即校验自洽性\n",
         &cmd_package},

        {"run", "跨平台启动器（替代 run*.bat）：注入 dll/ 到 PATH 后启动宿主",
         "用法: GryceGC run (--package <dir> | --build-dir <dir> [--project <dir>]) [--api ...] [--play] [-- <exe args>]\n"
         "选项:\n"
         "  --package <dir>     发布包目录（含 run.bat 的目录；资源在包内 demo_project/）\n"
         "  --build-dir <dir>   构建目录（相对路径以引擎根为基；缺省=当前目录）\n"
         "  --project <dir>     项目根，会作为 --project 传给宿主（定位 res:/ 与 main.js）\n"
         "  --api <api>         渲染后端 opengl|vulkan|dx12；缺省不传，由宿主按 project.data 决定\n"
         "  --play              交互模式（WASD + 鼠标）\n"
         "  --                  之后的参数原样透传给宿主\n"
         "说明: 宿主默认 GryceApp.exe（new 生成的用户项目）；--project 会被原样转发。\n",
         &cmd_run},

        {"copyright", "版权/许可印章：COPYRIGHT.txt + README 页脚 + PE trailer（+ VERSIONINFO）",
         "用法: GryceGC copyright --package <dir>|--exe <file> --holder <name> [options]\n"
         "选项:\n"
         "  --holder <name>     版权持有者（必填）\n"
         "  --year <yyyy>       年份（默认 2026）\n"
         "  --license <id>      SPDX 或自定义 license 标识（如 MIT）\n"
         "  --license-file <f>  license 全文文件（追加进 COPYRIGHT.txt）\n"
         "  --version-info      同时写 PE VERSIONINFO 资源（Windows）\n"
         "  --product <name>    VERSIONINFO ProductName\n"
         "  --version <ver>     VERSIONINFO ProductVersion\n",
         &cmd_copyright},

        {"meta", "元数据读写：gryce.meta.json / project.data / PE trailer",
         "用法: GryceGC meta <set|get|show|stamp-git> [options]\n"
         "选项:\n"
         "  --key <k>           键名（set/get 必填）\n"
         "  --value <v>         值（set 必填）\n"
         "  --target <path>     目录 / PE 文件（默认当前目录）\n"
         "  --project           目标按 <target>/project.data 处理\n"
         "  stamp-git           写入 git HEAD 短戳与提交日期\n",
         &cmd_meta},

        {"encrypt", "加密文件（AES-256-GCM / ChaCha20；引擎 aes_gcm_buf 兼容）",
         "用法: GryceGC encrypt --in <f> --out <f> --key <spec> [options]\n"
         "选项:\n"
         "  --algo <a>          aes-gcm（默认）| chacha20\n"
         "  --aad <text>        附加认证数据（默认空）\n"
         "  --raw               不写容器头（引擎兼容裸 iv||ct||tag）\n"
         "  --key <spec>        64 位 hex / @文件 / env:VAR / prompt:提示语\n",
         &cmd_encrypt},

        {"decrypt", "解密文件（自动识别 GRYCENC1 容器；无容器按 --algo 裸格式）",
         "用法: GryceGC decrypt --in <f> --out <f> --key <spec> [--algo a] [--aad t]\n",
         &cmd_decrypt},

        {"keygen", "生成密码学安全随机密钥（默认 32 字节）",
         "用法: GryceGC keygen [--bytes N] [--hex] [--out <file>]\n",
         &cmd_keygen},

        {"pak", "资源包 create/list/extract/verify（.gpack GPAK v1 / .pak v3+v4）",
         "用法: GryceGC pak <create|list|extract|verify> [options]\n"
         "  create  --out <f> [输入文件或目录...] [--key hex]   # --key 仅 .pak(v4 加密)\n"
         "  list    --file <f> [--key hex]\n"
         "  extract --file <f> --out <dir> [--key hex]\n"
         "  verify  --file <f> [--key hex]                      # CRC32 校验\n",
         &cmd_pak},

        {"debug", "以预设的调试环境变量启动宿主（d3d12 / trace / probe / glog）",
         "用法: GryceGC debug --package <dir>|--build-dir <dir>|--exe <f> [options]\n"
         "选项:\n"
         "  --d3d12             GRYCE_D3D12_DEBUG=1\n"
         "  --gpu-validation    GRYCE_D3D12_GPU_VALIDATION=1\n"
         "  --serialize         GRYCE_D3D12_SERIALIZE=1\n"
         "  --trace-hdr         GRYCE_TRACE_HDR=1\n"
         "  --trace-frame N     GRYCE_DX12_TRACE_FRAME=N\n"
         "  --trace-bind        GRYCE_DX12_TRACE_BIND=1\n"
         "  --probe-ssr         GRYCE_DX12_PROBE_SSR=1\n"
         "  --probe-frame N     GRYCE_DX12_PROBE_FRAME=N\n"
         "  --probe-cbv NAME    GRYCE_DX12_PROBE_CBV=NAME\n"
         "  --ring              GRYCE_DX12_PROBE_RING=1\n"
         "  --glog N            GLOG_minloglevel=N\n"
         "  --with-debugger D   cdb|windbg|x64dbg\n"
         "  --api <api>         opengl|vulkan|dx12\n",
         &cmd_debug},

        {"bench", "帧耗时 A/B 基准（移植 bench_ab.sh，输出单行 KV）",
         "用法: GryceGC bench --build-dir <dir> --w N --h N [--runs 3] [--api opengl]\n",
         &cmd_bench},

        {"font", "重新生成 engine/render/imgui_font_data.h",
         "用法: GryceGC font [源字体.ttf] [--out engine/render/imgui_font_data.h]\n"
         "默认源: third_party/imgui/misc/fonts/Cousine-Regular.ttf\n",
         &cmd_font},

        {"build", "cmake configure + build 封装（复用主工程工具链）",
         "用法: GryceGC build [options]\n"
         "说明: 构建的是引擎本体（GryceApp / GryceRenderSmoke / 测试 / DLL），不是用户项目；\n"
         "      源码根 = --source 指定，或从当前目录向上找含 CMakeLists.txt 的引擎根（默认当前目录）。\n"
         "选项:\n"
         "  --source <dir>      引擎源码根（含 CMakeLists.txt）；缺省自动探测\n"
         "  --build-dir <dir>   构建目录（相对路径以源码根为基，默认 build）\n"
         "  --preset <p>        debug（默认）| release\n"
         "  --target <name>     构建目标（可重复）\n"
         "  --d3d12 <on|off>    D3D12 后端开关（默认 on）\n"
         "  --lto               开启 LTO\n"
         "  --test              构建后跑 CTest\n"
         "  --clean             构建前清空目录\n"
         "  --jobs N            并行度\n"
         "  --cc/--cxx <path>   覆盖编译器（默认强制 MSYS2 UCRT64 g++，绝不用 MSVC）\n",
         &cmd_build},

        {"webpack", "把 Emscripten 多文件 Web 产物打成一个自包含 HTML（file:// 双击即用）",
         "用法: GryceGC webpack [options]\n"
         "选项:\n"
         "  --build-dir <dir>   构建目录（默认 build-webgl）\n"
         "  --name <name>       应用基名 / Emscripten 输出前缀（默认 GryceRenderSmoke）\n"
         "  --html <file>       入口 html（默认 <build-dir>/<name>.html）\n"
         "  --js <file>         Emscripten JS（默认 <build-dir>/<name>.js）\n"
         "  --wasm <file>       wasm（默认 <build-dir>/<name>.wasm）\n"
         "  --data <file>       file packager 数据（默认 <build-dir>/<name>.data，可缺）\n"
         "  --output <file>     输出单文件 html（默认 <build-dir>/<name>.single.html）\n"
         "原理：内联 js，并经 Module.locateFile（.wasm → data: URL）与\n"
         "      Module.getPreloadedPackage（.data → ArrayBuffer）消除运行时 fetch。\n"
         "      链接后处理，不碰构建系统、不需要重新链接。\n",
         &cmd_webpack},
    };
    return table;
}

} // namespace

const std::vector<Command>& all_commands() { return kTable(); }

const Command* find_command(const std::string& name) {
    const auto& t = kTable();
    auto it = std::find_if(t.begin(), t.end(),
                           [&](const Command& c) { return c.name == name; });
    return it == t.end() ? nullptr : &*it;
}

} // namespace grycegc
