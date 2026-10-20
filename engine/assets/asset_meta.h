// asset_meta.h — 资产侧车文件（<asset>.meta）：为每个导入资产保存稳定 GUID
// 与导入设置。编辑器依赖 GUID 在文件重命名/移动后仍能维持引用（层级里的
// MeshRenderer、材质槽、场景引用都按 GUID 指向，而不是按路径）。
#pragma once

#include <cstdint>
#include <string>

#include "export.h"

namespace gryce_engine::assets {

// ---------------------------------------------------------------------------
// AssetMeta — 侧车文件内容。
//
// 文件格式：与源资产同目录的 `<asset>.<ext>.meta`（保留原扩展名，避免
// texture.png.meta 与 texture.meta 在只差扩展名的资产上互相覆盖），JSON，UTF-8。
//
// 兼容原则：解析时缺失字段一律取默认值，未知字段原样忽略 —— 侧车文件可能由
// 更新版本的编辑器写出，旧引擎读它不能报错，更不能丢弃整个设置。
// ---------------------------------------------------------------------------
struct GRYCE_API AssetMeta {
    // 稳定标识（32 位十六进制小写）。首次导入时生成，之后永不改变；
    // 场景/材质对资产的引用以它为准。
    std::string guid;

    // 源资产路径（相对工程根，仅作展示与诊断；重命名时引擎会更新它，
    // 但即使没更新也不影响 GUID 的有效性）。
    std::string source_path;

    // 导入器类别："mesh" / "texture" / "skinned" / "audio"。决定哪些设置生效。
    std::string importer;

    // ---- 网格导入设置 ----
    // 导入缩放：源文件单位 → 米。摄影测量模型常见以厘米导出，需 0.01。
    float scale = 1.0f;
    // 源文件上轴：0 = X-up，1 = Y-up（默认，无需转换），2 = Z-up。
    // Z-up 模型（多数 CAD / 3ds Max 导出）导入时绕 X 轴 -90 度转到 Y-up。
    int up_axis = 1;
    // 导入后是否生成 LOD 链（大模型占用更多导入时间与内存）。
    bool generate_lod = true;
    // 是否读取源文件自带材质（关闭后统一用默认材质，便于先用白模摆位）。
    bool import_materials = true;
    // 叠在 material_rules 推导结果上的粗糙度倍率（1 = 不调整）。
    float roughness_scale = 1.0f;

    // ---- 源文件指纹 ----
    // 用于判断侧车是否过期（源文件被替换但没重新导入）。0 表示未记录。
    std::int64_t source_mtime_ns = 0;
    std::int64_t source_size = 0;

    // 侧车格式版本。引擎写入时使用当前版本，读到更高版本只警告不拒绝。
    int version = 1;
};

// 侧车文件的当前格式版本。
inline constexpr int kAssetMetaVersion = 1;

// 由资产路径得到侧车文件路径（`path` + ".meta"）。
GRYCE_API std::string asset_meta_path(const std::string& asset_path);

// 读取侧车文件。文件不存在 / 解析失败返回 false（out 保持原样）。
GRYCE_API bool read_asset_meta(const std::string& asset_path, AssetMeta& out);

// 写出侧车文件（自动补目录、自动填 version）。失败返回 false。
GRYCE_API bool write_asset_meta(const std::string& asset_path, const AssetMeta& meta);

// 读取侧车；不存在时按源文件生成一份（含新 GUID 与源文件指纹）并写盘。
// 返回 true 表示 out 被填充（写盘失败不影响 out，只是下次还要再生成）。
GRYCE_API bool load_or_create_asset_meta(const std::string& asset_path, AssetMeta& out);

// 源文件指纹与侧车记录不一致（或侧车缺失）时为 true —— 编辑器据此提示重新导入。
GRYCE_API bool asset_meta_is_stale(const std::string& asset_path, const AssetMeta& meta);

// 生成一个新 GUID（32 位十六进制小写，基于随机数 + 时钟，不依赖平台 UUID 接口）。
GRYCE_API std::string generate_asset_guid();

// 把 AssetMeta 的导入设置折算成实际生效的参数（供导入器使用）。
// 返回 false 表示设置非法（例如 scale <= 0），此时应回退默认值并记日志。
GRYCE_API bool asset_meta_validate(const AssetMeta& meta);

} // namespace gryce_engine::assets
