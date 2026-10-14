#pragma once

// 子命令注册表：M1 阶段只注册已实现命令；后续 milestone 在 registry.cpp 中追加。

#include <string>
#include <vector>

#include "../cli.h"

namespace grycegc {

struct Command {
    std::string name;
    std::string summary;
    std::string help;        // 多行帮助文本（首行缩进对齐）
    int (*fn)(const Options&);
};

const std::vector<Command>& all_commands();
const Command* find_command(const std::string& name);

} // namespace grycegc
