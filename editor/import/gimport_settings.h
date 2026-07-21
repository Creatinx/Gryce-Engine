#pragma once

#include <string>

namespace gryce_engine::editor {

// ---------------------------------------------------------------------------
// GImportSettings — 资源导入设置（保存在同目录 .gimport 文件中）
// ---------------------------------------------------------------------------
// 当前支持模型导入参数：
//   - scale: 统一缩放
//   - generate_collider: 是否生成碰撞体
//   - add_rigidbody: 是否添加刚体
//   - physics_material: 物理材质预设名（空表示不添加 PhysicalMaterial）
// ---------------------------------------------------------------------------

struct GImportSettings {
    float scale = 1.0f;
    bool generate_collider = true;
    bool add_rigidbody = false;
    std::string physics_material;
};

// 根据源资源路径推断 .gimport 文件路径，读取设置；文件不存在时返回默认设置。
GImportSettings load_gimport_settings(const std::string& source_path);

// 保存导入设置到源资源同目录的 .gimport 文件。
void save_gimport_settings(const std::string& source_path, const GImportSettings& settings);

// 若 .gimport 文件不存在，则使用默认设置创建一个。
GImportSettings ensure_gimport_settings(const std::string& source_path);

} // namespace gryce_engine::editor
