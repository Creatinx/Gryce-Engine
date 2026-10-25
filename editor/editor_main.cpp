// ===========================================================================
// editor/editor_main.cpp — 编辑器可执行入口
//
// 存在的理由：编辑器本来就是"一个项目目录"（main.js + 场景 + extension.dll），
// 此前只能靠通用宿主带参数启动：
//     GryceApp.exe --project editor/project
// 每次都要记住这串参数，IDE 的运行配置里也得手填一遍，而漏填时宿主会退回
// 当前工作目录去解析 res:/，报的是"场景加载失败"这种指向错误位置的消息。
//
// 这里把项目根烧进二进制，启动变成 build/GryceEditor.exe。实现仍然走同一个
// 通用宿主（gryce_engine::app::run），没有另起一套窗口/场景/退出逻辑 —— 编辑器
// 与 demo 一样是"宿主 + 项目"，宿主的行为对两者必须完全一致。
//
// 参数仍然可覆盖：下面把 --project 插在**最前面**，宿主按顺序解析、后出现
// 的同名参数覆盖先出现的，所以
//     GryceEditor.exe --project D:/somewhere/else
// 依然能指到别的工程副本（对比两个工程、跑被复制的场景时要用）。这样不需要
// 给通用宿主加任何"默认值"机制。
// ===========================================================================

#include "app/app_host.h"

#include <string>
#include <vector>

// 由 CMake 传入（见 editor/CMakeLists.txt）。带绝对路径：编辑器是开发期工具，
// 构建树与源码树分离时要能直接找到项目根。
#ifndef GRYCE_EDITOR_PROJECT_ROOT
#error "GRYCE_EDITOR_PROJECT_ROOT not defined: build through CMake"
#endif

int main(int argc, char** argv) {
    std::vector<std::string> args;
    args.reserve(static_cast<size_t>(argc) + 2);
    args.emplace_back(argv[0]);
    args.emplace_back("--project");
    args.emplace_back(GRYCE_EDITOR_PROJECT_ROOT);
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }

    // std::string::data() 自 C++17 起返回 char*，宿主只读不改写。
    std::vector<char*> raw;
    raw.reserve(args.size());
    for (std::string& a : args) {
        raw.push_back(a.data());
    }

    return gryce_engine::app::run(static_cast<int>(raw.size()), raw.data());
}