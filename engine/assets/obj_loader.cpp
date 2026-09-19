#include "obj_loader.h"

#include <cmath>
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

namespace {

std::string dir_of(const std::string& path) {
    const size_t pos = path.find_last_of("/\\");
    return (pos == std::string::npos) ? std::string() : path.substr(0, pos + 1);
}

std::string trim(const std::string& s) {
    const size_t begin = s.find_first_not_of(" \t\r");
    if (begin == std::string::npos) return {};
    const size_t end = s.find_last_not_of(" \t\r");
    return s.substr(begin, end - begin + 1);
}

// 解析 MTL 文件中指定 newmtl 段的材质字段（相对贴图路径基于 MTL 所在目录）。
void load_mtl(const std::string& mtl_path, const std::string& material_name, MeshMaterialData& out) {
    std::ifstream file(mtl_path);
    if (!file.is_open()) {
        GLOG_WARN("ObjLoader: cannot open MTL '{}'", mtl_path);
        return;
    }

    const std::string base_dir = dir_of(mtl_path);
    bool in_section = false;
    float ns = -1.0f;
    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        std::istringstream iss(line);
        std::string key;
        iss >> key;

        if (key == "newmtl") {
            std::string name;
            iss >> name;
            // 进入目标段；离开目标段即结束
            if (in_section) break;
            in_section = (name == material_name);
            continue;
        }
        if (!in_section) continue;

        if (key == "Kd") {
            iss >> out.albedo_color.x >> out.albedo_color.y >> out.albedo_color.z;
        } else if (key == "Ke") {
            iss >> out.emissive_color.x >> out.emissive_color.y >> out.emissive_color.z;
        } else if (key == "d") {
            iss >> out.opacity;
        } else if (key == "Tr") {
            float tr = 1.0f;
            iss >> tr;
            out.opacity = 1.0f - tr;
        } else if (key == "Ns") {
            iss >> ns;
        } else if (key == "map_Kd") {
            std::string p;
            std::getline(iss, p);
            out.albedo_map = base_dir + trim(p);
        } else if (key == "map_Ke") {
            std::string p;
            std::getline(iss, p);
            out.emissive_map = base_dir + trim(p);
        } else if (key == "map_Bump" || key == "bump" || key == "norm") {
            std::string p;
            std::getline(iss, p);
            out.normal_map = base_dir + trim(p);
        }
    }

    if (!in_section) return;

    // Blinn 高光指数转 GGX 粗糙度（常见近似）
    if (ns >= 0.0f) {
        out.roughness = std::sqrt(2.0f / (ns + 2.0f));
        if (out.roughness > 1.0f) out.roughness = 1.0f;
    }
    out.valid = true;
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

    std::string mtl_lib;
    std::string use_mtl;

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
        } else if (prefix == "mtllib") {
            iss >> mtl_lib;
        } else if (prefix == "usemtl") {
            // 当前加载器把全部面合并为单一 MeshData，只取第一个材质
            if (use_mtl.empty()) {
                iss >> use_mtl;
            }
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
        if (!mtl_lib.empty() && !use_mtl.empty()) {
            load_mtl(dir_of(path) + mtl_lib, use_mtl, data.material);
        }
        result.push_back(std::move(data));
    }

    GLOG_INFO("ObjLoader: loaded '{}' ({} vertices, {} indices)",
              path, result.empty() ? 0 : result[0].vertices.size(),
              result.empty() ? 0 : result[0].indices.size());
    return result;
}

} // namespace gryce_engine::assets
