#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace gryce_engine::editor {

// ---------------------------------------------------------------------------
// 平台小工具：UTF-8 路径转换 / 在系统文件管理器中显示文件
// ---------------------------------------------------------------------------

// Windows 上 std::filesystem 的窄字符串→宽字符转换使用系统代码页（GBK），
// UTF-8 字符串（如中文文件名）会被误判为非法序列，触发 CRT "无效参数" 致命错误。
// 统一显式按 UTF-8 转换。
std::filesystem::path utf8_path(const std::string& utf8);

// 反向转换：磁盘上的路径 → UTF-8 显示字符串。
// MSVC 的 path.string() 按系统代码页（GBK）转换，ImGui 按 UTF-8 渲染会乱码。
std::string path_to_utf8(const std::filesystem::path& p);

// 在系统文件管理器中显示该文件/文件夹（Windows：explorer /select）。
// 异步调用，不阻塞编辑器。
void reveal_in_file_explorer(const std::filesystem::path& path);

// 文件修改时间 → "YYYY-MM-DD HH:MM:SS"（本地时间）；失败返回空串
std::string format_file_time(const std::filesystem::path& path);

// 字节数 → "512 B" / "1.5 KB" / "2.30 MB" 形式
std::string format_file_size(std::uintmax_t bytes);

} // namespace gryce_engine::editor
