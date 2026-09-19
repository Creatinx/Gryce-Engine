#pragma once

// ResourceLoader — 发布模式资源加载器
//
// 职责：
// 1. 挂载 .gpkg / .pak（复用 PakReader，GPAK v3 随机 Base64 名称 + manifest 映射）
// 2. 按逻辑路径读取原始资源（纹理、模型、音频等，不打密）
// 3. 解密发布包中加密的 JS 字节码与 DSL 文本（AES-256-GCM）
//
// 加密块格式（pack_* 生成，decrypt_* 读取）：
//   [AES-256-GCM 单缓冲] = iv(12) || ciphertext || tag(16)
//   载荷前 4 字节为魔数，用于识别类型与校验解密正确性：
//     "GJSB" -> JS 字节码
//     "GUIT" -> DSL 文本
//
// 密钥由多个拆分常量运行时异或组合得到（仅防普通用户，非安全边界）。

#include "../export.h"
#include "pak_bundle.h"

#include <cstdint>
#include <string>
#include <vector>

namespace gryce_engine::resources {

// 加密块魔数（打包与运行时共用）
inline constexpr char kChunkMagicJsBytecode[4] = {'G', 'J', 'S', 'B'};
inline constexpr char kChunkMagicUiText[4]     = {'G', 'U', 'I', 'T'};

// 获取 32 字节主密钥（拆分常量异或组合，打包器与运行时共用）
GRYCE_API void resource_master_key(uint8_t out[32]);

class GRYCE_API ResourceLoader {
public:
    // 挂载一个 .gpkg / .pak；失败返回 false（沿用 PakReader 的 GPAK v1/v3 兼容）
    bool mount(const std::string& pak_path);

    bool is_mounted() const { return reader_.is_open(); }
    const std::string& path() const { return reader_.path(); }

    // 原始读取（不打密），找不到返回空
    std::vector<uint8_t> read_raw(const std::string& logical_path) const;

    // 解密读取（GCM tag 校验失败 / 魔数不符返回空，不崩溃）
    std::string decrypt_ui_text(const std::string& logical_path) const;
    std::vector<uint8_t> decrypt_js_bytecode(const std::string& logical_path) const;

    // -----------------------------------------------------------------------
    // 打包辅助（grycegc --pak 与测试共用；与解密方法对称）
    // -----------------------------------------------------------------------

    // JS 字节码 -> 加密块（GJSB + AES-GCM）
    static std::vector<uint8_t> pack_js_bytecode(const std::vector<uint8_t>& bytecode);
    // DSL 文本 -> 加密块（GUIT + AES-GCM）
    static std::vector<uint8_t> pack_ui_text(const std::string& text);
    // 原始数据原样返回（dev 模式 / 非加密资源）
    static std::vector<uint8_t> pack_raw(const std::vector<uint8_t>& data);

private:
    // 从已挂载包读取逻辑路径对应的加密块并解密（不校验魔数）
    std::vector<uint8_t> decrypt_chunk(const std::string& logical_path) const;

    mutable PakReader reader_;
};

} // namespace gryce_engine::resources
