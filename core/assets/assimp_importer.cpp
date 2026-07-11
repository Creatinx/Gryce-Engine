#include "assimp_importer.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include "utils/glog/glog_lib.h"

namespace gryce_engine::assets {

namespace {

math::Vector3f to_vector3f(const aiVector3D& v) {
    return math::Vector3f(v.x, v.y, v.z);
}

math::Vector2f to_vector2f(const aiVector3D& v) {
    return math::Vector2f(v.x, v.y);
}

MeshData process_mesh(aiMesh* mesh) {
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

    return data;
}

void process_node(aiNode* node, const aiScene* scene, std::vector<MeshData>& out) {
    for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
        aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
        out.push_back(process_mesh(mesh));
    }
    for (unsigned int i = 0; i < node->mNumChildren; ++i) {
        process_node(node->mChildren[i], scene, out);
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

    process_node(scene->mRootNode, scene, result);

    std::size_t total_vertices = 0;
    for (const auto& m : result) total_vertices += m.vertices.size();
    GLOG_INFO("AssimpImporter: loaded '{}' ({} meshes, {} total vertices)",
              path, result.size(), total_vertices);
    return result;
}

} // namespace gryce_engine::assets
