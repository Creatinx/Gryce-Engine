#pragma once

#include <string>
#include <vector>

#include "assets/mesh_data.h"

namespace gryce_engine::assets {

// ---------------------------------------------------------------------------
// AssimpImporter — 基于 Assimp 的模型导入器
// 支持 OBJ/FBX/glTF/DAE/PLY/STL。
// ---------------------------------------------------------------------------
class AssimpImporter {
public:
    std::vector<MeshData> import(const std::string& path) const;
};

} // namespace gryce_engine::assets
