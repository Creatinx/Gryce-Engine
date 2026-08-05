#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace gryce_engine {
namespace editor {

// ---------------------------------------------------------------------------
// AssetDatabase — 编辑器资源数据库
//
// 为项目中的每个资源文件维护一个 .meta 文件，记录全局唯一 GUID 与资源类型。
// 提供 path <-> GUID 双向查询，并在 Project 面板等位置提供资源元数据。
//
// .meta 文件格式（JSON，与源资源同名同目录）：
// {
//   "guid": "550e8400-e29b-41d4-a716-446655440000",
//   "type": "texture"
// }
//
// 当前资源类型从扩展名推断，不强制用户填写。
// ---------------------------------------------------------------------------
class AssetDatabase {
public:
    static AssetDatabase& instance();

    // 扫描项目资源根目录，为没有 .meta 的文件生成 .meta
    void scan(const std::filesystem::path& project_root);

    // 强制重新扫描并清空缓存
    void rescan(const std::filesystem::path& project_root);

    // 获取/创建单个资源的元数据；返回的 guid 保证非空
    std::string ensure_meta(const std::filesystem::path& file_path);

    // path -> guid（相对或绝对路径均可；找不到返回空串）
    std::string guid_for_path(const std::filesystem::path& path) const;

    // guid -> 绝对路径（找不到返回空串）
    std::string path_for_guid(const std::string& guid) const;

    // 根据扩展名推断资源类型（texture, mesh, material, scene, prefab, audio, unknown）
    static std::string infer_type(const std::filesystem::path& path);

    // 返回最近一次扫描的项目根
    const std::filesystem::path& root() const { return root_; }

private:
    AssetDatabase() = default;

    std::filesystem::path root_;
    std::unordered_map<std::string, std::string> guid_to_path_;
    std::unordered_map<std::string, std::string> path_to_guid_;

    void load_or_create_meta(const std::filesystem::path& file_path);
    static std::filesystem::path meta_path_for(const std::filesystem::path& file_path);
    static std::string generate_guid();
};

} // namespace editor
} // namespace gryce_engine
