// import_options.cpp — 把侧车文件里的导入设置作用到已导入的网格上。
// 只有 OpenGL / Assimp 两条导入路径共用这一份实现，缩放与上轴才不会出现
// 同一工程里 OBJ 与 FBX 错位的问题。
#include "assets/import_options.h"

#include <algorithm>

#include "math/math.h"

namespace gryce_engine::assets {

void apply_import_options(std::vector<MeshData>& meshes, const ImportOptions& options) {
    // scale <= 0 视为非法设置，直接跳过缩放而不是把模型缩成零体积。
    const bool do_scale = options.global_scale != 1.0f && options.global_scale > 0.0f;
    const bool do_axis = options.up_axis != 1;

    for (MeshData& mesh : meshes) {
        if (do_scale) {
            // 只缩放位置：均匀正缩放不改变法线/切线方向，无需重算。
            for (MeshVertex& v : mesh.vertices) v.position *= options.global_scale;
        }
        if (do_axis) {
            // 上轴转换到引擎约定的 Y-up：
            //   Z-up → 绕 X 轴 -90 度，(x, y, z) → (x, z, -y)
            //   X-up → 绕 Z 轴 +90 度，(x, y, z) → (-y, x, z)
            // 顶点、法线、切线必须一起转，否则法线贴图的方向会整体偏 90 度。
            const bool z_up = options.up_axis == 2;
            for (MeshVertex& v : mesh.vertices) {
                const math::Vector3f p = v.position;
                const math::Vector3f n = v.normal;
                const math::Vector3f t = v.tangent;
                if (z_up) {
                    v.position = math::Vector3f(p.x, p.z, -p.y);
                    v.normal = math::Vector3f(n.x, n.z, -n.y);
                    v.tangent = math::Vector3f(t.x, t.z, -t.y);
                } else {
                    v.position = math::Vector3f(-p.y, p.x, p.z);
                    v.normal = math::Vector3f(-n.y, n.x, n.z);
                    v.tangent = math::Vector3f(-t.y, t.x, t.z);
                }
            }
        }
        if (!options.import_materials) {
            // valid = false 让 MeshRenderer 走组件默认材质，等价于白模摆位。
            mesh.material = MeshMaterialData{};
        } else if (options.roughness_scale != 1.0f) {
            mesh.material.roughness =
                std::clamp(mesh.material.roughness * options.roughness_scale, 0.0f, 1.0f);
        }
    }
}

} // namespace gryce_engine::assets
