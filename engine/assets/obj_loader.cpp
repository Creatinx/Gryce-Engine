#include "obj_loader.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <vector>

#include "assets/material_rules.h"
#include "assets/texture_path.h"
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

bool is_number(const std::string& token) {
    if (token.empty()) return false;
    char* end = nullptr;
    std::strtod(token.c_str(), &end);
    return end != nullptr && *end == '\0';
}

// map_* 行的选项参数个数（"-o 1 2 3" / "-mm 0 1" / "-clamp on" …）。
// 不跳过这些选项的话，贴图路径会被解析成 "-o"、"on" 之类的垃圾字符串。
int map_option_arg_count(const std::string& option, const std::vector<std::string>& rest,
                         std::size_t pos) {
    if (option == "-o" || option == "-s" || option == "-t") {
        // 最多 3 个数值，实际可能只给 1~2 个
        int count = 0;
        for (; count < 3 && pos + 1 + static_cast<std::size_t>(count) < rest.size(); ++count) {
            if (!is_number(rest[pos + 1 + static_cast<std::size_t>(count)])) break;
        }
        return count;
    }
    if (option == "-mm") return 2;
    if (option == "-halo") return 0;
    return 1;  // -blendu/-blendv/-bm/-boost/-cc/-clamp/-imfchan/-texres/-type
}

// 解析 map_Kd / map_Bump 之类的行，返回贴图文件在该行里的原始写法。
std::string parse_map_reference(const std::string& args) {
    std::istringstream iss(args);
    std::vector<std::string> tokens;
    std::string token;
    while (iss >> token) tokens.push_back(token);

    std::size_t i = 0;
    while (i < tokens.size() && tokens[i].size() > 1 && tokens[i][0] == '-') {
        const int arg_count = map_option_arg_count(tokens[i], tokens, i);
        i += 1 + static_cast<std::size_t>(arg_count < 0 ? 0 : arg_count);
    }
    if (i >= tokens.size()) return {};

    // 文件名可能含空格（"wall stone.jpg"）：剩下的整体拼回去
    std::string filename = tokens[i];
    for (std::size_t k = i + 1; k < tokens.size(); ++k) {
        filename += " ";
        filename += tokens[k];
    }
    return filename;
}

// ---------------------------------------------------------------------------
// MTL —— 解析整份材质库
//
// 一个 OBJ 可以挂多个 mtllib，材质名也可能重名（后出现的覆盖先出现的）。
// 解析结果直接是引擎侧的 MeshMaterialData：spec-gloss 字段交给
// material_rules 统一推导，和 assimp 路径的规则完全一致。
// ---------------------------------------------------------------------------
void parse_mtl_file(const std::string& mtl_path,
                    std::unordered_map<std::string, MeshMaterialData>& out) {
    std::ifstream file(mtl_path);
    if (!file.is_open()) {
        GLOG_WARN("ObjLoader: cannot open MTL '{}'", mtl_path);
        return;
    }
    const std::string mtl_base_dir = dir_of(mtl_path);

    SurfaceInput input;
    std::string current;
    bool in_material = false;
    // 贴图槽位暂存，全部读完后统一 strdup 到 MeshMaterialData
    std::string albedo_map, normal_map, emissive_map, roughness_map, metallic_map, ao_map,
        specular_map;

    auto flush = [&]() {
        if (!in_material || current.empty()) return;
        input.has_base_color_map = !albedo_map.empty();
        input.has_specular_map = !specular_map.empty();
        // 只给了自发光贴图（map_Ke）、没给 Ke 时按白色处理，否则颜色乘 0 看不见
        if (!emissive_map.empty() && surface_luminance(input.emissive) < 1e-4f) {
            input.emissive = math::Vector3f::one();
        }
        const SurfaceResult surface = resolve_surface(input);

        MeshMaterialData data;
        data.valid = true;
        data.name = current;
        data.surface_type = surface.type;
        data.albedo_color = surface.base_color;
        data.roughness = surface.roughness;
        data.metallic = surface.metallic;
        data.opacity = surface.opacity;
        data.emissive_color = surface.emissive;
        data.two_sided = surface.two_sided;

        auto resolve_map = [&](const std::string& raw) {
            if (raw.empty()) return std::string();
            return texture_path::resolve(mtl_base_dir, raw);
        };
        data.albedo_map = resolve_map(albedo_map);
        data.normal_map = resolve_map(normal_map);
        data.emissive_map = resolve_map(emissive_map);
        data.roughness_map = resolve_map(roughness_map);
        data.metallic_map = resolve_map(metallic_map);
        data.ao_map = resolve_map(ao_map);
        const std::string specular_resolved = resolve_map(specular_map);
        // 镜面工作流里的金属：没有漫反射贴图时，镜面贴图就是它的基色贴图
        if (data.albedo_map.empty() && surface.metallic > 0.5f && !specular_resolved.empty()) {
            data.albedo_map = specular_resolved;
        }

        GLOG_INFO("ObjLoader: material '{}' -> {} (roughness {}, metallic {}, opacity {}, albedo_map={}, normal_map={})",
                  data.name, surface_type_name(surface.type), data.roughness, data.metallic,
                  data.opacity, !data.albedo_map.empty(), !data.normal_map.empty());

        out[current] = std::move(data);

        // 重置，准备下一个 newmtl
        input = SurfaceInput{};
        albedo_map.clear();
        normal_map.clear();
        emissive_map.clear();
        roughness_map.clear();
        metallic_map.clear();
        ao_map.clear();
        specular_map.clear();
    };

    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        std::istringstream iss(line);
        std::string key;
        iss >> key;

        if (key == "newmtl") {
            flush();
            iss >> current;
            in_material = true;
            continue;
        }
        if (!in_material) continue;

        if (key == "Kd") {
            iss >> input.base_color.x >> input.base_color.y >> input.base_color.z;
        } else if (key == "Ks") {
            iss >> input.specular_color.x >> input.specular_color.y >> input.specular_color.z;
            input.has_specular = true;
        } else if (key == "Ke") {
            iss >> input.emissive.x >> input.emissive.y >> input.emissive.z;
        } else if (key == "Ns") {
            float ns = 0.0f;
            if (iss >> ns) {
                input.has_shininess = true;
                input.shininess = ns;
            }
        } else if (key == "d") {
            float d = 1.0f;
            if (iss >> d) {
                input.has_opacity = true;
                input.opacity = d;
            }
        } else if (key == "Tr") {
            // 部分导出器把透明度写成 Tr（= 1 - d）
            float tr = 0.0f;
            if (iss >> tr) {
                input.has_opacity = true;
                input.opacity = 1.0f - tr;
            }
        } else if (key == "Pr") {
            // PBR 扩展（MtlX / 主流 DCC 导出）：直接给粗糙度
            float pr = 0.0f;
            if (iss >> pr) {
                input.has_roughness = true;
                input.roughness = pr;
            }
        } else if (key == "Pm") {
            float pm = 0.0f;
            if (iss >> pm) {
                input.has_metallic = true;
                input.metallic = pm;
            }
        } else if (key == "illum") {
            // illum 0 = 只给颜色（无光照），1/2 = 漫反射+高光，3+ = 反射/透明
            // 只用来判断"是否声明了镜面项"：illum 0/1 的材料 Ks 不可信。
            int illum = 1;
            if (iss >> illum && illum <= 1) {
                input.has_specular = false;
            }
        } else if (key == "map_Kd") {
            std::string rest;
            std::getline(iss, rest);
            albedo_map = parse_map_reference(rest);
        } else if (key == "map_Ke") {
            std::string rest;
            std::getline(iss, rest);
            emissive_map = parse_map_reference(rest);
        } else if (key == "map_Ks") {
            std::string rest;
            std::getline(iss, rest);
            specular_map = parse_map_reference(rest);
        } else if (key == "map_Pr") {
            std::string rest;
            std::getline(iss, rest);
            roughness_map = parse_map_reference(rest);
        } else if (key == "map_Pm") {
            std::string rest;
            std::getline(iss, rest);
            metallic_map = parse_map_reference(rest);
        } else if (key == "map_ao" || key == "map_Ka") {
            std::string rest;
            std::getline(iss, rest);
            ao_map = parse_map_reference(rest);
        } else if (key == "map_Bump" || key == "bump" || key == "norm" || key == "map_bump") {
            std::string rest;
            std::getline(iss, rest);
            normal_map = parse_map_reference(rest);
        } else if (key == "map_d") {
            // 透明度贴图：引擎的 PBR 用 albedo 的 alpha 通道，独立 alpha 图暂不支持
            GLOG_INFO("ObjLoader: material '{}' has an alpha map, ignored (use albedo alpha)",
                      current);
        }
    }
    flush();
}

// 单个材质分组（OBJ 的 usemtl 分段）
struct SurfaceGroup {
    std::string material;
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
};

} // namespace

std::vector<MeshData> ObjLoader::load(const std::string& path) const {
    std::vector<MeshData> result;

    std::ifstream file(path);
    if (!file.is_open()) {
        GLOG_ERROR("ObjLoader: cannot open '{}'", path);
        return result;
    }

    std::vector<math::Vector3f> positions;
    std::vector<math::Vector3f> normals;
    std::vector<math::Vector2f> uvs;

    std::vector<std::string> mtl_libs;
    std::vector<SurfaceGroup> groups;                       // 保序
    std::unordered_map<std::string, std::size_t> group_of;  // 材质名 → groups 下标

    // 当前写入的分组（首次遇到 usemtl 时才建组）
    SurfaceGroup* current = nullptr;
    auto select_group = [&](const std::string& material) {
        auto it = group_of.find(material);
        if (it != group_of.end()) {
            current = &groups[it->second];
            return;
        }
        group_of.emplace(material, groups.size());
        groups.push_back(SurfaceGroup{material, {}, {}});
        current = &groups.back();
    };

    std::string line;
    while (std::getline(file, line)) {
        // 去掉行尾残留的 '\r'：仓库层面已用 .gitattributes 锁定 LF 出库，
        // 但若检出环境把它改写成 CRLF，最后的字段会被污染（典型症状是
        // "mtllib xxx.mtl\r" 导致贴图库打开失败）。这里做一次兜底。
        line = trim(line);
        if (line.empty()) continue;

        std::istringstream iss(line);
        std::string prefix;
        iss >> prefix;

        if (prefix.empty() || prefix[0] == '#') continue;

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
            // mtllib 后面可能跟多个文件名；带空格的文件名走 "引号" 或整体当一个名字
            std::string rest;
            std::getline(iss, rest);
            rest = trim(rest);
            if (!rest.empty()) {
                if (rest.front() == '"' && rest.back() == '"' && rest.size() > 1) {
                    mtl_libs.push_back(rest.substr(1, rest.size() - 2));
                } else {
                    mtl_libs.push_back(rest);
                }
            }
        } else if (prefix == "usemtl") {
            std::string name;
            std::getline(iss, name);
            select_group(trim(name));
        } else if (prefix == "f") {
            if (!current) select_group(std::string());  // 没有 usemtl 的裸面片
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
                    face_indices.push_back(static_cast<uint32_t>(current->vertices.size()));
                    current->vertices.push_back(v);
                }
            }
            // 三角化：扇形拆分
            for (std::size_t i = 2; i < face_indices.size(); ++i) {
                current->indices.push_back(face_indices[0]);
                current->indices.push_back(face_indices[i - 1]);
                current->indices.push_back(face_indices[i]);
            }
        }
    }

    // 材质库：obj 目录 + 每个 mtllib（不存在时按文件名在常见子目录里找）
    std::unordered_map<std::string, MeshMaterialData> materials;
    const std::string base_dir = dir_of(path);
    for (const std::string& lib : mtl_libs) {
        const std::string mtl_path = texture_path::resolve(base_dir, lib);
        if (!mtl_path.empty()) {
            parse_mtl_file(mtl_path, materials);
            continue;
        }
        // 一行里写了多个材质库（"mtllib a.mtl b.mtl"）时按空格再拆一次。
        // 先整体试是为了不破坏含空格的文件名（"mtllib stone wall.mtl"）。
        std::istringstream iss(lib);
        std::string token;
        bool any = false;
        while (iss >> token) {
            const std::string one = texture_path::resolve(base_dir, token);
            if (one.empty()) continue;
            parse_mtl_file(one, materials);
            any = true;
        }
        if (!any) GLOG_WARN("ObjLoader: mtllib '{}' of '{}' not found", lib, path);
    }

    result.reserve(groups.size());
    for (SurfaceGroup& group : groups) {
        if (group.vertices.empty()) continue;
        MeshData data;
        data.name = group.material.empty() ? path : (path + "#" + group.material);
        data.vertices = std::move(group.vertices);
        data.indices = std::move(group.indices);
        compute_tangents(data);
        if (!group.material.empty()) {
            auto it = materials.find(group.material);
            if (it != materials.end()) {
                data.material = it->second;
            } else {
                GLOG_INFO("ObjLoader: usemtl '{}' has no entry in mtllib", group.material);
            }
        }
        result.push_back(std::move(data));
    }

    std::size_t total_vertices = 0;
    for (const auto& m : result) total_vertices += m.vertices.size();
    GLOG_INFO("ObjLoader: loaded '{}' ({} material group(s), {} vertices)",
              path, result.size(), total_vertices);
    return result;
}

} // namespace gryce_engine::assets
