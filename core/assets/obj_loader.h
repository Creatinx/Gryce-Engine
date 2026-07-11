#pragma once

#include <string>
#include <vector>

#include "assets/mesh_data.h"

namespace gryce_engine::assets {

// ---------------------------------------------------------------------------
// ObjLoader — 内建 Wavefront OBJ 加载器
// 当 Assimp 不可用时提供基础模型导入能力。
// ---------------------------------------------------------------------------
class ObjLoader {
public:
    std::vector<MeshData> load(const std::string& path) const;
};

} // namespace gryce_engine::assets
