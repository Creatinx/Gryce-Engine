#include "obj_loader.h"

#include <cstdio>
#include <fstream>
#include <sstream>

#include "utils/glog/glog_lib.h"

namespace gryce_engine::assets {

namespace {

void compute_tangents(MeshData& data) {
    if (data.indices.empty()) return;

    // 每个三角面计算 tangent 并累加到三个顶点
    for (std::size_t i = 0; i + 2 < data.indices.size(); i += 3) {
        MeshVertex& v0 = data.vertices[data.indices[i]];
        MeshVertex& v1 = data.vertices[data.indices[i + 1]];
        MeshVertex& v2 = data.vertices[data.indices[i + 2]];

        math::Vector3f edge1 = v1.position - v0.position;
        math::Vector3f edge2 = v2.position - v0.position;
        math::Vector2f delta_uv1 = v1.uv - v0.uv;
        math::Vector2f delta_uv2 = v2.uv - v0.uv;

        float f = 1.0f / (delta_uv1.x * delta_uv2.y - delta_uv2.x * delta_uv1.y + 1e-6f);
        math::Vector3f tangent = math::Vector3f(
            f * (delta_uv2.y * edge1.x - delta_uv1.y * edge2.x),
            f * (delta_uv2.y * edge1.y - delta_uv1.y * edge2.y),
            f * (delta_uv2.y * edge1.z - delta_uv1.y * edge2.z)
        ).normalized();

        v0.tangent = v0.tangent + tangent;
        v1.tangent = v1.tangent + tangent;
        v2.tangent = v2.tangent + tangent;
    }

    // 正交化并归一化
    for (auto& v : data.vertices) {
        v.tangent = (v.tangent - v.normal * v.normal.dot(v.tangent)).normalized();
    }
}

} // namespace

std::vector<MeshData> ObjLoader::load(const std::string& path) const {
    std::vector<MeshData> result;

    std::ifstream file(path);
    if (!file.is_open()) {
        GLOG_ERROR("ObjLoader: cannot open '{}'", path);
        return result;
    }

    MeshData data;
    data.name = path;

    std::vector<math::Vector3f> positions;
    std::vector<math::Vector3f> normals;
    std::vector<math::Vector2f> uvs;

    std::string line;
    while (std::getline(file, line)) {
        std::istringstream iss(line);
        std::string prefix;
        iss >> prefix;

        if (prefix == "v") {
            float x, y, z;
            iss >> x >> y >> z;
            positions.emplace_back(x, y, z);
        } else if (prefix == "vn") {
            float x, y, z;
            iss >> x >> y >> z;
            normals.emplace_back(x, y, z);
        } else if (prefix == "vt") {
            float u, v;
            iss >> u >> v;
            uvs.emplace_back(u, v);
        } else if (prefix == "f") {
            std::vector<uint32_t> face_indices;
            std::string vertex_str;
            while (iss >> vertex_str) {
                // 支持 f v/vt/vn、v//vn、v/vt、v
                int vi = 0, ti = 0, ni = 0;
                if (std::sscanf(vertex_str.c_str(), "%d/%d/%d", &vi, &ti, &ni) >= 1 ||
                    std::sscanf(vertex_str.c_str(), "%d//%d", &vi, &ni) >= 1 ||
                    std::sscanf(vertex_str.c_str(), "%d/%d", &vi, &ti) >= 1) {
                    // OBJ 索引从 1 开始
                    MeshVertex v;
                    v.position = (vi > 0 && vi <= static_cast<int>(positions.size()))
                                     ? positions[vi - 1]
                                     : math::Vector3f::zero();
                    v.normal = (ni > 0 && ni <= static_cast<int>(normals.size()))
                                   ? normals[ni - 1]
                                   : math::Vector3f::up();
                    v.uv = (ti > 0 && ti <= static_cast<int>(uvs.size()))
                               ? uvs[ti - 1]
                               : math::Vector2f::zero();
                    v.color = math::Vector3f::one();
                    face_indices.push_back(static_cast<uint32_t>(data.vertices.size()));
                    data.vertices.push_back(v);
                }
            }
            // 三角化：扇形拆分
            for (std::size_t i = 2; i < face_indices.size(); ++i) {
                data.indices.push_back(face_indices[0]);
                data.indices.push_back(face_indices[i - 1]);
                data.indices.push_back(face_indices[i]);
            }
        }
    }

    if (!data.empty()) {
        compute_tangents(data);
        result.push_back(std::move(data));
    }

    GLOG_INFO("ObjLoader: loaded '{}' ({} vertices, {} indices)",
              path, result.empty() ? 0 : result[0].vertices.size(),
              result.empty() ? 0 : result[0].indices.size());
    return result;
}

} // namespace gryce_engine::assets
