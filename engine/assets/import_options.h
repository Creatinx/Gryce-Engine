// import_options.h — 模型导入期可调参数（来源是资产的侧车文件，见
// assets/asset_meta.h）。独立成一个小模块而不是塞进 AssimpImporter：内置
// OBJ 加载器与 Assimp 路径必须共用同一份缩放/上轴实现，而 OBJ 路径在没有
// Assimp 的构建里同样要能用。
#pragma once

#include <vector>

#include "export.h"
#include "assets/mesh_data.h"

namespace gryce_engine::assets {

// ---------------------------------------------------------------------------
// ImportOptions — 默认值等价于"老行为"：不缩放、假定 Y-up、读取文件材质、
// 粗糙度不调整。因此不读侧车文件时导入结果与引入本模块之前完全一致。
// ---------------------------------------------------------------------------
struct ImportOptions {
    // 源文件单位 → 米。摄影测量 / CAD 常见厘米导出，需要 0.01。
    float global_scale = 1.0f;
    // 源上轴：0 = X-up，1 = Y-up（不转换），2 = Z-up（导入时绕 X 轴 -90 度）。
    int up_axis = 1;
    // 是否采用文件自带材质；false 时全部退回默认材质（白模摆位用）。
    bool import_materials = true;
    // 叠在 material_rules 推导结果上的粗糙度倍率（1 = 不调整）。
    float roughness_scale = 1.0f;
};

// 把导入设置作用到已导入的网格上：缩放、上轴转换、材质开关、粗糙度倍率。
// 就地修改，不改变子网格数量与顺序。
GRYCE_API void apply_import_options(std::vector<MeshData>& meshes, const ImportOptions& options);

} // namespace gryce_engine::assets
