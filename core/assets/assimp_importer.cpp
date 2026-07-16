#include "assimp_importer.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include <cmath>

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

} // namespace gryce_engine::assets
