#include "assimp_importer.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include <cmath>
#include <functional>
#include <unordered_map>
#include <unordered_set>

#include "utils/glog/glog_lib.h"

namespace gryce_engine::assets {

namespace {

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

// 从 aiMaterial 提取 PBR 材质字段（贴图路径基于模型文件目录解析）。
void process_material(const aiScene* scene, aiMesh* mesh, const std::string& base_dir, MeshData& data) {
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
        if (mat->GetTexture(type, 0, &tex_path) == AI_SUCCESS) {
            out_path = base_dir + tex_path.C_Str();
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

MeshData process_mesh(const aiScene* scene, aiMesh* mesh, const std::string& base_dir) {
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

    data.indices.reserve(static_cast<std::size_t>(mesh->mNumFaces) * 3);
    for (unsigned int i = 0; i < mesh->mNumFaces; ++i) {
        const aiFace& face = mesh->mFaces[i];
        for (unsigned int j = 0; j < face.mNumIndices; ++j) {
            data.indices.push_back(face.mIndices[j]);
        }
    }

    process_material(scene, mesh, base_dir, data);
    return data;
}

void process_node(aiNode* node, const aiScene* scene, const std::string& base_dir, std::vector<MeshData>& out) {
    for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
        aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
        out.push_back(process_mesh(scene, mesh, base_dir));
    }
    for (unsigned int i = 0; i < node->mNumChildren; ++i) {
        process_node(node->mChildren[i], scene, base_dir, out);
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

    process_node(scene->mRootNode, scene, dir_of(path), result);

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
    std::function<void(aiNode*)> collect_meshes = [&](aiNode* node) {
        for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
            aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
            SkinnedMeshData data;
            static_cast<MeshData&>(data) = process_mesh(scene, mesh, base_dir);

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
