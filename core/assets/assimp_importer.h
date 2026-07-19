#pragma once

#include <string>
#include <vector>

#include "assets/mesh_data.h"
#include "assets/skinned_mesh_data.h"

namespace gryce_engine::assets {

// ---------------------------------------------------------------------------
// AssimpImporter — 基于 Assimp 的模型导入器
// 支持 OBJ/FBX/glTF/DAE/PLY/STL。
// ---------------------------------------------------------------------------
class AssimpImporter {
public:
    std::vector<MeshData> import(const std::string& path) const;

    // 骨骼动画导入：在常规网格提取之外，额外解析
    // skeleton（node 层级 + aiBone offset matrix）、
    // 顶点骨骼权重（aiBone/aiVertexWeight，top-4 截断 + 归一化）、
    // 动画剪辑（aiAnimation 的 aiNodeAnim 关键帧，时间换算为秒）。
    // 纯 CPU 操作，可在 AsyncLoader 工作线程调用。
    // 文件不含蒙皮数据时 has_skin=false，meshes 退化为普通网格。
    SkinnedModelData import_skinned(const std::string& path) const;
};

} // namespace gryce_engine::assets
