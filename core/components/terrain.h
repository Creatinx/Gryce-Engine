#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "components/component.h"
#include "assets/mesh_data.h"
#include "math/math.h"

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// Terrain — 基础地形组件（M2 基础版）
//
// 使用规则高度图网格描述 3D 地形，支持程序化生成和简单参数编辑。
// 当前版本以数据持有为主：编辑器通过 TerrainEditorWindow 可视化编辑高度图，
// 生成 MeshData 后可由外部系统保存为模型资源或附加 MeshRenderer 渲染。
// ---------------------------------------------------------------------------
class Terrain : public Component {
public:
    // 地形平面尺寸（世界单位）
    float width = 100.0f;
    float depth = 100.0f;

    // 网格分辨率：每边的顶点数 - 1；值越大网格越密
    int resolution = 64;

    // 高度缩放：高度图值 [0,1] 映射到 [0, height_scale]
    float height_scale = 10.0f;

    // 基础纹理路径（空表示使用默认材质）
    std::string base_texture_path;

    // 程序化生成种子；0 表示使用默认起伏
    int seed = 0;

    Terrain() = default;

    const char* type() const override { return "Terrain"; }

    void serialize(nlohmann::json& out) const override;
    void deserialize(const nlohmann::json& in) override;

    // 高度图维度（resolution+1）
    int heightmap_size() const;

    // 获取/设置高度图采样（x,z 为 0..resolution 的网格索引）
    float height_at(int x, int z) const;
    void set_height(int x, int z, float h);

    // 规范化高度图到 [0,1]
    void normalize_heights();

    // 使用简单噪声重新生成高度图
    void generate_noise();

    // 将当前高度图构建为 MeshData（含法线/切线/UV）
    assets::MeshData build_mesh_data() const;

private:
    std::vector<float> heightmap_;

    void ensure_heightmap();
    math::Vector3f compute_normal(int x, int z) const;
};

} // namespace gryce_engine::components
