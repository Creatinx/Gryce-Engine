// 字符串与文件名工具。
#pragma once

#include <string>

namespace gryce_engine::gc {

// 小写化整个字符串。
std::string to_lower(std::string s);

// 规范化默认项目名：非字母数字字符作为分词符，每词首字母大写，如 "ecs_demo" -> "EcsDemo"。
// 显式传入的 --name 不经此函数，始终保持原样。
std::string to_pascal_case(std::string s);

// 将字符串转为 JSON 字面量转义（引号/反斜杠/控制字符）。
std::string json_escape(const std::string& s);

} // namespace gryce_engine::gc