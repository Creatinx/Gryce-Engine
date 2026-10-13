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

        {"deps", "列出可执行文件的完整 DLL 依赖闭包（按 core / 依赖分类）",
         "用法: GryceGC deps [options]\n"
         "选项:\n"
         "  --build-dir <dir>   CMake 构建目录（默认 cmake-build-debug）\n"
         "  --exe <path>        可执行文件路径或名称（默认在 build-dir 内自动探测）\n"
         "  --extra-dir <dir>   额外 DLL 搜索目录（可重复）\n",
         &cmd_deps},

        {"verify", "校验发布包自洽性：所有导入 DLL 在包内可解析",
         "用法: GryceGC verify --package <dir> [--dll-folder <name>]\n"
         "选项:\n"
         "  --package <dir>     发布包目录（必填）\n"
         "  --dll-folder <name> 依赖 DLL 所在子目录名（默认 dll）\n",
         &cmd_verify},

        // 以下子命令在后续 milestone 中接入；当前直接调用会提示"未实现"
        {"package", "[M2] 打包成可脱机运行的发布目录",
         "用法: GryceGC package [options]（M2 milestone 待实现）\n",
         &cmd_package},
        {"run", "[M2] 跨平台启动器（替代 run*.bat）",
         "用法: GryceGC run [options] -- [exe args]（M2 milestone 待实现）\n",
         &cmd_run},
        {"copyright", "[M2] 在包/二进制上盖版权印章",
         "用法: GryceGC copyright [options]（M2 milestone 待实现）\n",
         &cmd_copyright},
        {"meta", "[M2] 元数据读写（gryce.meta.json / project.data / PE 尾）",
         "用法: GryceGC meta set|show|get [options]（M2 milestone 待实现）\n",
         &cmd_meta},
        {"encrypt", "[M3] AES-256-GCM / ChaCha20 加解密（兼容引擎 aes_gcm_buf）",
         "用法: GryceGC encrypt [options]（M3 milestone 待实现）\n",
         &cmd_encrypt},
        {"decrypt", "[M3] 对应 decrypt 命令（输入格式由 --algo 决定）",
         "用法: GryceGC decrypt [options]（M3 milestone 待实现）\n",
         &cmd_decrypt},
        {"keygen", "[M3] 生成密码学安全随机密钥（默认 32 字节）",
         "用法: GryceGC keygen --out <file> [--hex]（M3 milestone 待实现）\n",
         &cmd_keygen},
        {"pak", "[M3] 资源包 create/list/extract/verify（.gpack / .pak）",
         "用法: GryceGC pak <sub> [options]（M3 milestone 待实现）\n",
         &cmd_pak},
        {"debug", "[M4] 以预设的调试环境变量启动宿主（d3d12 / trace / probe）",
         "用法: GryceGC debug [options]（M4 milestone 待实现）\n",
         &cmd_debug},
        {"bench", "[M4] 移植 bench_ab.sh：Debug/Release 帧耗时 A/B 基准",
         "用法: GryceGC bench --build-dir <dir> --w N --h N [--runs 3]（M4 milestone 待实现）\n",
         &cmd_bench},
        {"font", "[M4] 重新生成 engine/render/imgui_font_data.h",
         "用法: GryceGC font [源字体.ttf]（M4 milestone 待实现）\n",
         &cmd_font},
        {"build", "[M5] cmake configure + build 封装",
         "用法: GryceGC build [options]（M5 milestone 待实现）\n",
         &cmd_build},
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
