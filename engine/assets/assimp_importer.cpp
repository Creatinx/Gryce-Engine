#include "assimp_importer.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/texture.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "utils/glog/glog_lib.h"

namespace gryce_engine::assets {

namespace {

namespace fs = std::filesystem;

math::Vector3f to_vector3f(const aiVector3D& v) {
    return math::Vector3f(v.x, v.y, v.z);
}

math::Vector2f to_vector2f(const aiVector3D& v) {
    return math::Vector2f(v.x, v.y);
}

std::string dir_of(const std::string& path) {
    const size_t pos = path.find_last_of("/\\");
    return (pos == std::string::npos) ? std::string() : path.substr(0, pos + 1);
}

// FBX 等格式的贴图路径带反斜杠，统一归一化为 '/'（Windows 同样接受）。
std::string normalize_separators(std::string path) {
    for (char& c : path) {
        if (c == '\\') c = '/';
    }
    return path;
}

// 内嵌贴图（aiScene::mTextures）落盘缓存目录。
// 贴图加载链路（Material::load_texture_from_path → AssetManager）只认磁盘路径，
// 因此把内嵌数据解出到临时目录后按普通文件路径引用。
fs::path embedded_texture_cache_dir() {
    static const fs::path dir = fs::temp_directory_path() / "gryce-embedded-textures";
    return dir;
}

// 压缩内嵌贴图（achFormatHint 为空时按 magic bytes 兜底判定扩展名）。
const char* sniff_compressed_extension(const uint8_t* data, size_t size) {
    if (size >= 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') return "png";
    if (size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) return "jpg";
    if (size >= 2 && data[0] == 'B' && data[1] == 'M') return "bmp";
    if (size >= 18 && std::memcmp(data + size - 18, "TRUEVISION-XFILE.", 17) == 0) return "tga";
    return nullptr;
}

// 非压缩 aiTexel（b,g,r,a 四字节）→ 未压缩 TGA32（同为 BGRA 字节序，可整块拷贝）。
// 18 字节 TGA 头 + width*height*4 像素；descriptor 0x28 = 原点在左上、8 位 alpha。

// TGA 头的宽/高是 16-bit 字段，尺寸超出后头里的值与实际写出的数据不一致，
// 下游解码器按头读取会得到截断、错乱的像素。
constexpr uint32_t k_max_tga_dimension = 0xFFFFu;
// 单张内嵌贴图落盘上限：尺寸/字节数都来自不可信的文件，需要一个绝对天花板，
// 否则恶意模型可以让 importing 线程往临时目录灌十几 GB 数据。
constexpr uint64_t k_max_embedded_texture_bytes = 256ull * 1024u * 1024u;

bool write_uncompressed_tga(const fs::path& path, const aiTexture* tex) {
    const uint32_t w = tex->mWidth;
    const uint32_t h = tex->mHeight;
    if (w == 0 || h == 0) return false;
    if (w > k_max_tga_dimension || h > k_max_tga_dimension) {
        GLOG_WARN("AssimpImporter: embedded texture {}x{} exceeds TGA 16-bit dimension limit, skipped", w, h);
        return false;
    }

    // 64-bit 计算总字节数：32-bit 下 w*h*4 会回绕（例如 0x10000*0x10000*4 → 0），
    // 回绕后写出的字节数远小于头声明的尺寸，下游会读到越界像素。
    const uint64_t pixel_bytes = static_cast<uint64_t>(w) * static_cast<uint64_t>(h) * 4ull;
    if (pixel_bytes > k_max_embedded_texture_bytes) {
        GLOG_WARN("AssimpImporter: embedded texture {}x{} needs {} bytes, over the 256 MiB cap, skipped",
                  w, h, pixel_bytes);
        return false;
    }

    std::ofstream out(path, std::ios::binary);
    if (!out) return false;

    uint8_t header[18] = {0};
    header[2] = 2;                       // datatypecode: 未压缩真彩色
    header[12] = static_cast<uint8_t>(w & 0xFF);
    header[13] = static_cast<uint8_t>((w >> 8) & 0xFF);
    header[14] = static_cast<uint8_t>(h & 0xFF);
    header[15] = static_cast<uint8_t>((h >> 8) & 0xFF);
    header[16] = 32;                     // bits per pixel
    header[17] = 0x28;                   // top-left origin + 8 alpha bits
    out.write(reinterpret_cast<const char*>(header), sizeof(header));

    out.write(reinterpret_cast<const char*>(tex->pcData),
              static_cast<std::streamsize>(pixel_bytes));
    if (!out.good()) {
        out.close();
        // 不留下半截文件：写失败后残留的损坏 TGA 会被下一次导入当成有效缓存命中。
        std::error_code ec;
        fs::remove(path, ec);
        return false;
    }
    return true;
}

// 提取 scene->mTextures[idx] 到缓存目录，返回落盘路径；失败返回空串。
// cache 以贴图索引为键去重：同一 scene 内多个材质引用同一内嵌贴图只写一次盘。
std::string extract_embedded_texture(const aiScene* scene, unsigned int index,
                                     std::unordered_map<unsigned int, std::string>& cache) {
    if (index >= scene->mNumTextures) return {};

    auto it = cache.find(index);
    if (it != cache.end()) return it->second;

    const aiTexture* tex = scene->mTextures[index];
    if (!tex || !tex->pcData) return {};

    std::error_code ec;
    fs::create_directories(embedded_texture_cache_dir(), ec); // 已存在时静默

    fs::path path;
    if (tex->mHeight == 0) {
        // 压缩数据：mWidth 为字节数，原样落盘
        const auto* data = reinterpret_cast<const uint8_t*>(tex->pcData);
        const size_t size = tex->mWidth;
        if (size == 0) return {};

        std::string ext;
        if (tex->achFormatHint[0] != '\0') {
            ext = tex->achFormatHint;
        } else if (const char* sniffed = sniff_compressed_extension(data, size)) {
            ext = sniffed;
        } else {
            GLOG_WARN("AssimpImporter: embedded texture {} has unknown compressed format", index);
            return {};
        }

        path = embedded_texture_cache_dir() / ("embedded_" + std::to_string(index) + "." + ext);
        std::ofstream out(path, std::ios::binary);
        if (!out) {
            GLOG_ERROR("AssimpImporter: cannot open '{}' for embedded texture", path.string());
            return {};
        }
        out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        if (!out.good()) {
            GLOG_ERROR("AssimpImporter: failed writing embedded texture '{}'", path.string());
            return {};
        }
    } else {
        // 非压缩 ARGB8888 像素数组
        path = embedded_texture_cache_dir() / ("embedded_" + std::to_string(index) + ".tga");
        if (!write_uncompressed_tga(path, tex)) {
            GLOG_ERROR("AssimpImporter: failed decoding embedded texture '{}'", path.string());
            return {};
        }
    }

    cache.emplace(index, path.string());
    return path.string();
}

// 从 aiMaterial 提取 PBR 材质字段（贴图路径基于模型文件目录解析）。
// "*N" 形式的路径指向 aiScene::mTextures 内嵌贴图，提取落盘后重定向。
void process_material(const aiScene* scene, aiMesh* mesh, const std::string& base_dir,
                      std::unordered_map<unsigned int, std::string>& embedded_cache,
                      MeshData& data) {
    if (!scene->HasMaterials() || mesh->mMaterialIndex >= scene->mNumMaterials) return;
    const aiMaterial* mat = scene->mMaterials[mesh->mMaterialIndex];

    aiColor3D color;
    if (mat->Get(AI_MATKEY_COLOR_DIFFUSE, color) == AI_SUCCESS) {
        data.material.albedo_color = math::Vector3f(color.r, color.g, color.b);
    }
    if (mat->Get(AI_MATKEY_COLOR_EMISSIVE, color) == AI_SUCCESS) {
        data.material.emissive_color = math::Vector3f(color.r, color.g, color.b);
    }
    float opacity = 1.0f;
    if (mat->Get(AI_MATKEY_OPACITY, opacity) == AI_SUCCESS) {
        data.material.opacity = opacity;
    }
    float shininess = -1.0f;
    if (mat->Get(AI_MATKEY_SHININESS, shininess) == AI_SUCCESS && shininess >= 0.0f) {
        // Blinn 高光指数转 GGX 粗糙度（常见近似）
        data.material.roughness = std::sqrt(2.0f / (shininess + 2.0f));
        if (data.material.roughness > 1.0f) data.material.roughness = 1.0f;
    }
    float metallic = 0.0f;
    if (mat->Get(AI_MATKEY_METALLIC_FACTOR, metallic) == AI_SUCCESS) {
        data.material.metallic = metallic;
    }

    auto get_texture = [&](aiTextureType type, std::string& out_path) {
        if (mat->GetTextureCount(type) == 0) return;
        aiString tex_path;
        if (mat->GetTexture(type, 0, &tex_path) != AI_SUCCESS) return;

        const char* raw = tex_path.C_Str();
        if (raw[0] == '*') {
            // 内嵌贴图引用："*N" → aiScene::mTextures[N]
            const unsigned int index = static_cast<unsigned int>(std::strtoul(raw + 1, nullptr, 10));
            out_path = extract_embedded_texture(scene, index, embedded_cache);
            return;
        }
        out_path = normalize_separators(base_dir + raw);
        // FBX 等格式常记录导出机器的绝对路径（换机后必然失效）：
        // 拼接口径不存在时，回退用文件名在模型目录查找（贴图通常与模型同目录分发）。
        std::error_code ec;
        if (!fs::exists(out_path, ec)) {
            const fs::path fallback = fs::path(base_dir) / fs::path(raw).filename();
            if (fallback != out_path && fs::exists(fallback, ec)) {
                out_path = normalize_separators(fallback.string());
            }
        }
    };
    get_texture(aiTextureType_DIFFUSE, data.material.albedo_map);
    get_texture(aiTextureType_NORMALS, data.material.normal_map);
    get_texture(aiTextureType_EMISSIVE, data.material.emissive_map);
    get_texture(aiTextureType_DIFFUSE_ROUGHNESS, data.material.roughness_map);
    get_texture(aiTextureType_METALNESS, data.material.metallic_map);
    get_texture(aiTextureType_AMBIENT_OCCLUSION, data.material.ao_map);

    data.material.valid = true;
}

MeshData process_mesh(const aiScene* scene, aiMesh* mesh, const std::string& base_dir,
                      std::unordered_map<unsigned int, std::string>& embedded_cache) {
    MeshData data;
    data.name = mesh->mName.C_Str();

    data.vertices.reserve(mesh->mNumVertices);
    for (unsigned int i = 0; i < mesh->mNumVertices; ++i) {
        MeshVertex v;
        v.position = to_vector3f(mesh->mVertices[i]);
        v.normal = mesh->HasNormals() ? to_vector3f(mesh->mNormals[i]) : math::Vector3f::up();
        v.tangent = mesh->HasTangentsAndBitangents() ? to_vector3f(mesh->mTangents[i]) : math::Vector3f::right();
        v.uv = (mesh->mTextureCoords[0] != nullptr) ? to_vector2f(mesh->mTextureCoords[0][i]) : math::Vector2f::zero();
        v.color = math::Vector3f::one();
        data.vertices.push_back(v);
    }

    // 面数来自不可信文件：32-bit 下 mNumFaces*3 会回绕（如 0x60000000*3 → 0），
    // 回绕后 reserve 容量失真、push 反复重分配；总量上限同时防恶意文件撑爆内存。
    constexpr uint64_t k_max_index_count = 256ull * 1024 * 1024;  // ≈ 1 GB (uint32)
    const uint64_t expected_indices = static_cast<uint64_t>(mesh->mNumFaces) * 3ull;
    if (expected_indices > k_max_index_count) {
        GLOG_WARN("AssimpImporter: mesh '{}' declares {} faces ({} indices), over cap; indices dropped",
                  data.name, mesh->mNumFaces, expected_indices);
        return data;  // 只保留顶点，空索引网格渲染时 draw 0 个图元，安全
    }
    data.indices.reserve(static_cast<std::size_t>(expected_indices));
    for (unsigned int i = 0; i < mesh->mNumFaces; ++i) {
        const aiFace& face = mesh->mFaces[i];
        for (unsigned int j = 0; j < face.mNumIndices; ++j) {
            const unsigned int idx = face.mIndices[j];
            // 索引值同样来自文件：越界索引会让 GPU/下游读到垃圾地址（段错误）。
            // 出现第一个越界值就丢弃整个网格的索引 —— 半截索引会拼出错乱三角形。
            if (idx >= mesh->mNumVertices) {
                GLOG_WARN("AssimpImporter: mesh '{}' face {} has out-of-range index {} ({} vertices); indices dropped",
                          data.name, i, idx, mesh->mNumVertices);
                data.indices.clear();
                return data;
            }
            data.indices.push_back(idx);
        }
    }

    process_material(scene, mesh, base_dir, embedded_cache, data);
    return data;
}

void process_node(aiNode* node, const aiScene* scene, const std::string& base_dir,
                  std::unordered_map<unsigned int, std::string>& embedded_cache,
                  std::vector<MeshData>& out) {
    for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
        aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
        out.push_back(process_mesh(scene, mesh, base_dir, embedded_cache));
    }
    for (unsigned int i = 0; i < node->mNumChildren; ++i) {
        process_node(node->mChildren[i], scene, base_dir, embedded_cache, out);
    }
}

} // namespace

std::vector<MeshData> AssimpImporter::import(const std::string& path) const {
    std::vector<MeshData> result;

    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(path,
        aiProcess_Triangulate |
        aiProcess_FlipUVs |
        aiProcess_GenNormals |
        aiProcess_CalcTangentSpace);

    if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) {
        GLOG_ERROR("AssimpImporter: failed to load '{}': {}", path, importer.GetErrorString());
        return result;
    }

    std::unordered_map<unsigned int, std::string> embedded_cache;
    process_node(scene->mRootNode, scene, dir_of(path), embedded_cache, result);

    std::size_t total_vertices = 0;
    for (const auto& m : result) total_vertices += m.vertices.size();
    GLOG_INFO("AssimpImporter: loaded '{}' ({} meshes, {} total vertices)",
              path, result.size(), total_vertices);
    return result;
}

// ---------------------------------------------------------------------------
// 骨骼动画导入
// ---------------------------------------------------------------------------
namespace {

// aiMatrix4x4（行主序，a1..d4 按行排列）→ Matrix4f（列主序 m[col*4+row]）
math::Matrix4f to_matrix4f(const aiMatrix4x4& m) {
    math::Matrix4f out;
    const float* src = &m.a1;
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            out(row, col) = src[row * 4 + col];
        }
    }
    return out;
}

// aiQuaternion 成员顺序为 (w, x, y, z)，引擎为 (x, y, z, w)
math::Quaternionf to_quaternionf(const aiQuaternion& q) {
    return math::Quaternionf(q.x, q.y, q.z, q.w);
}

// 标记"被 aiBone 引用的节点及其全部祖先"（后序：子节点有需要则父节点也需要）。
// 祖先节点是骨骼层级的一部分（动画/全局 pose 链路经过它们），必须入 skeleton。
bool mark_needed_nodes(aiNode* node, const std::unordered_set<std::string>& bone_names,
                       std::unordered_set<aiNode*>& needed) {
    bool required = bone_names.count(node->mName.C_Str()) > 0;
    for (unsigned int i = 0; i < node->mNumChildren; ++i) {
        required |= mark_needed_nodes(node->mChildren[i], bone_names, needed);
    }
    if (required) needed.insert(node);
    return required;
}

// 先序遍历给被标记节点分配 bone index（天然满足拓扑序：父先于子），
// 并从 aiNode::mTransformation 分解 bind local TRS。
void build_skeleton_bones(aiNode* node, const std::unordered_set<aiNode*>& needed,
                          int32_t parent_index, animation::Skeleton& skeleton,
                          std::unordered_map<std::string, int32_t>& name_to_index) {
    int32_t my_index = parent_index;
    if (needed.count(node) > 0) {
        animation::Bone bone;
        bone.name = node->mName.C_Str();
        bone.parent_index = parent_index;

        aiVector3D scaling, position;
        aiQuaternion rotation;
        node->mTransformation.Decompose(scaling, rotation, position);
        bone.bind_position = to_vector3f(position);
        bone.bind_rotation = to_quaternionf(rotation).normalized();
        bone.bind_scale = to_vector3f(scaling);

        my_index = static_cast<int32_t>(skeleton.bones.size());
        // 同名节点（导出器偶发）保留先序的第一个
        name_to_index.emplace(bone.name, my_index);
        skeleton.bones.push_back(std::move(bone));
    }
    for (unsigned int i = 0; i < node->mNumChildren; ++i) {
        build_skeleton_bones(node->mChildren[i], needed, my_index, skeleton, name_to_index);
    }
}

} // namespace

SkinnedModelData AssimpImporter::import_skinned(const std::string& path) const {
    SkinnedModelData model;

    Assimp::Importer importer;
    // 与 import() 相同的 post-process 标志；蒙皮权重随顶点保留，不受影响
    const aiScene* scene = importer.ReadFile(path,
        aiProcess_Triangulate |
        aiProcess_FlipUVs |
        aiProcess_GenNormals |
        aiProcess_CalcTangentSpace);

    if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) {
        GLOG_ERROR("AssimpImporter: failed to load '{}': {}", path, importer.GetErrorString());
        return model;
    }

    const std::string base_dir = dir_of(path);

    // 1. 收集全部 mesh 引用的骨骼名（并集）
    std::unordered_set<std::string> bone_names;
    for (unsigned int i = 0; i < scene->mNumMeshes; ++i) {
        const aiMesh* mesh = scene->mMeshes[i];
        for (unsigned int b = 0; b < mesh->mNumBones; ++b) {
            bone_names.insert(mesh->mBones[b]->mName.C_Str());
        }
    }

    // 2. 构建 skeleton（无骨骼引用则跳过，退化为普通模型）
    std::unordered_map<std::string, int32_t> name_to_index;
    if (!bone_names.empty()) {
        std::unordered_set<aiNode*> needed;
        mark_needed_nodes(scene->mRootNode, bone_names, needed);
        build_skeleton_bones(scene->mRootNode, needed, -1, model.skeleton, name_to_index);

        // aiBone::mOffsetMatrix 即 inverse bind matrix，直接采用
        std::vector<bool> has_offset(model.skeleton.bone_count(), false);
        for (unsigned int i = 0; i < scene->mNumMeshes; ++i) {
            const aiMesh* mesh = scene->mMeshes[i];
            for (unsigned int b = 0; b < mesh->mNumBones; ++b) {
                const aiBone* bone = mesh->mBones[b];
                auto it = name_to_index.find(bone->mName.C_Str());
                if (it == name_to_index.end()) continue;
                const int32_t idx = it->second;
                model.skeleton.bones[idx].inverse_bind_matrix = to_matrix4f(bone->mOffsetMatrix);
                has_offset[static_cast<size_t>(idx)] = true;
            }
        }

        // 无 aiBone 的结构祖先节点：inverse bind = inverse(global bind)
        {
            std::vector<math::Matrix4f> global_bind(model.skeleton.bone_count());
            for (size_t i = 0; i < model.skeleton.bone_count(); ++i) {
                const auto& bone = model.skeleton.bones[i];
                const math::Matrix4f local = bone.bind_local_matrix();
                global_bind[i] = (bone.parent_index < 0)
                                     ? local
                                     : global_bind[static_cast<size_t>(bone.parent_index)] * local;
                if (!has_offset[i]) {
                    model.skeleton.bones[i].inverse_bind_matrix = global_bind[i].inverse();
                }
            }
        }
    }

    // 3. 提取 mesh（复用常规路径）+ 顶点骨骼权重
    std::unordered_map<unsigned int, std::string> embedded_cache;
    std::function<void(aiNode*)> collect_meshes = [&](aiNode* node) {
        for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
            aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
            SkinnedMeshData data;
            static_cast<MeshData&>(data) = process_mesh(scene, mesh, base_dir, embedded_cache);

            if (mesh->mNumBones > 0 && !name_to_index.empty()) {
                data.bone_influences.resize(data.vertices.size());
                for (unsigned int b = 0; b < mesh->mNumBones; ++b) {
                    const aiBone* bone = mesh->mBones[b];
                    auto it = name_to_index.find(bone->mName.C_Str());
                    if (it == name_to_index.end()) continue;
                    const int32_t bone_index = it->second;
                    for (unsigned int w = 0; w < bone->mNumWeights; ++w) {
                        const aiVertexWeight& vw = bone->mWeights[w];
                        if (vw.mVertexId < data.bone_influences.size()) {
                            data.bone_influences[vw.mVertexId].add(bone_index, vw.mWeight);
                        }
                    }
                }
                // top-4 截断后权重和不再为 1，逐顶点归一化
                for (auto& inf : data.bone_influences) {
                    inf.normalize();
                }
                model.has_skin = true;
            }
            model.meshes.push_back(std::move(data));
        }
        for (unsigned int i = 0; i < node->mNumChildren; ++i) {
            collect_meshes(node->mChildren[i]);
        }
    };
    collect_meshes(scene->mRootNode);

    // 4. 提取动画剪辑（时间统一换算为秒）
    for (unsigned int a = 0; a < scene->mNumAnimations; ++a) {
        const aiAnimation* anim = scene->mAnimations[a];
        // mTicksPerSecond 可能为 0（未显式给出），此时 mTime/mDuration 即秒
        const double tps = (anim->mTicksPerSecond != 0.0) ? anim->mTicksPerSecond : 1.0;

        animation::AnimationClip clip;
        clip.name = anim->mName.C_Str();
        clip.ticks_per_second = static_cast<float>(tps);
        clip.duration = static_cast<float>(anim->mDuration / tps);

        for (unsigned int c = 0; c < anim->mNumChannels; ++c) {
            const aiNodeAnim* channel = anim->mChannels[c];
            auto it = name_to_index.find(channel->mNodeName.C_Str());
            // 非骨骼节点（如摄像机/灯光/网格节点）的 channel 跳过
            if (it == name_to_index.end()) continue;

            animation::BoneTrack track;
            track.bone_index = it->second;
            track.position_keys.reserve(channel->mNumPositionKeys);
            for (unsigned int k = 0; k < channel->mNumPositionKeys; ++k) {
                const aiVectorKey& key = channel->mPositionKeys[k];
                track.position_keys.push_back(
                    {static_cast<float>(key.mTime / tps), to_vector3f(key.mValue)});
            }
            track.rotation_keys.reserve(channel->mNumRotationKeys);
            for (unsigned int k = 0; k < channel->mNumRotationKeys; ++k) {
                const aiQuatKey& key = channel->mRotationKeys[k];
                track.rotation_keys.push_back(
                    {static_cast<float>(key.mTime / tps), to_quaternionf(key.mValue).normalized()});
            }
            track.scale_keys.reserve(channel->mNumScalingKeys);
            for (unsigned int k = 0; k < channel->mNumScalingKeys; ++k) {
                const aiVectorKey& key = channel->mScalingKeys[k];
                track.scale_keys.push_back(
                    {static_cast<float>(key.mTime / tps), to_vector3f(key.mValue)});
            }
            if (!track.empty()) {
                clip.tracks.push_back(std::move(track));
            }
        }
        model.animations.push_back(std::move(clip));
    }

    GLOG_INFO("AssimpImporter: loaded skinned '{}' ({} meshes, {} bones, {} clips, skin={})",
              path, model.meshes.size(), model.skeleton.bone_count(),
              model.animations.size(), model.has_skin);
    return model;
}

} // namespace gryce_engine::assets
