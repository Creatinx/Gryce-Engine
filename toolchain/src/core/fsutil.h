#pragma once

// ---------------------------------------------------------------------------
// GryceGC fsutil：跨平台文件/路径工具
// ---------------------------------------------------------------------------

#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <vector>

namespace grycegc {

// 安全地 mkdir -p；等价 std::filesystem::create_directories，但忽略 error_code 噪音
inline void mkdir_p(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
}

// 把 src 拷贝到 dst（覆盖）。使用 std::filesystem::copy_file 的 overwrite 形式。
inline bool copy_file(const std::filesystem::path& src, const std::filesystem::path& dst) {
    std::error_code ec;
    std::filesystem::copy_file(src, dst,
                               std::filesystem::copy_options::overwrite_existing, ec);
    return !ec;
}

// 递归拷贝目录（src → dst/dst_name）。dst 父目录须已存在。
bool copy_tree(const std::filesystem::path& src, const std::filesystem::path& dst);

// 把文本以二进制方式写入（保留 \r\n 不被翻译）
bool write_binary(const std::filesystem::path& p, const void* data, size_t len);
inline bool write_binary(const std::filesystem::path& p, const std::string& s) {
    return write_binary(p, s.data(), s.size());
}

// 读整文件为字节向量；失败返回空
std::vector<uint8_t> read_all(const std::filesystem::path& p);

// Windows 路径分隔符归一化为 '/'
std::string posix_path(const std::filesystem::path& p);

// 在 PATH 环境变量目录中找可执行；返回第一个存在的绝对路径或空
std::string which(const std::string& name);

// 当前时间戳（ISO 8601，本地时间）
std::string now_iso8601();

// 当前 user@host（用于 build_id 元数据）
std::string user_host();

// 临时目录（用于测试与中转文件）
std::filesystem::path temp_dir();

// 从当前目录向上查找第一个包含 CMakeLists.txt 的目录（引擎源码根）。
// 找不到返回空 path。用于 build / run / package 统一把"引擎根"作为基准，
// 这样无论从引擎根还是其任意子目录（含自身的 build/）调用都能定位到引擎。
std::filesystem::path find_engine_root();

// 解析默认构建目录：在引擎根下按序探测候选目录，取第一个存在的；
// 优先选含 GryceApp.exe 的（多候选并存时准确定位可打包的那个）。
// 全部不存在则返回第一个候选（调用方报错时至少给出约定俗成的名字）。
// base 为空时取 find_engine_root()（再退化到 cwd）。
std::filesystem::path find_build_dir(const std::vector<std::string>& candidates,
                                     const std::filesystem::path& base = {});

} // namespace grycegc
