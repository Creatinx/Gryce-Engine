#pragma once

// ---------------------------------------------------------------------------
// GryceGC pe_image：PE 导入表解析 + EOF trailer 读写
//
// 移植自 toolchain/resource.py 的 pe_import_dll_names()。
// 优势：
//   - 不依赖 objdump / binutils，纯自解析，CI 与离线场景均可用；
//   - 错误路径有界（不抛异常），失败返回空；
//   - trailer 设计为工具层叠加（不修改 PE 头/节表），保留二进制兼容。
// ---------------------------------------------------------------------------

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace grycegc::pe {

// 解析一个 PE 文件（.exe / .dll）直接导入的 DLL 名称列表。失败返回空。
// 大小写保留文件内写法，调用方自行 tolower。
std::vector<std::string> import_dll_names(const std::filesystem::path& path);

// 简要 PE 头信息（M1 阶段：machine + 32/64 位；M5 补 RT_VERSION 探测）
struct PeInfo {
    bool ok = false;
    bool is_pe32_plus = false;   // true = 64 位
    uint16_t machine = 0;
    std::string error;
};
PeInfo inspect(const std::filesystem::path& path);

// ---------------------------------------------------------------------------
// EOF trailer：用于在 PE 之外附加元数据 / 版权印章，不影响 PE 加载。
// 格式：GRYCETL1(8) + uint32_t payload_size + payload
// 写时如已存在 trailer 段，则就地替换。
// ---------------------------------------------------------------------------
inline constexpr const char* kTrailerMagic = "GRYCETL1";
inline constexpr size_t kTrailerMagicLen = 8;
inline constexpr size_t kTrailerHeaderLen = kTrailerMagicLen + 4;

struct Trailer {
    std::string payload;  // 任意字节；GryceGC 内部用 UTF-8 JSON
};

// 读取 trailer 文本（成功返回 true）；不存在也返回 true 但 trailer.payload 为空
bool read_trailer(const std::filesystem::path& path, Trailer& out);

// 写入或替换 trailer：读出原文件、裁掉已有 trailer、附加新 trailer。
// 失败返回 false（原文件保持原状）。
bool write_trailer(const std::filesystem::path& path, const Trailer& in);

// 去掉 trailer，返回去掉后剩余的字节（用于进一步处理）；无 trailer 返回空 vector
std::vector<uint8_t> strip_trailer(const std::filesystem::path& path);

} // namespace grycegc::pe
