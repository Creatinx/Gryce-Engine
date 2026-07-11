#include "asset_manager.h"

#include <algorithm>
#include <cctype>

#include "assets/obj_loader.h"
#ifdef GRYCE_HAS_ASSIMP
#include "assets/assimp_importer.h"
#endif
#include "resources/resource_path.h"
#include "utils/glog/glog_lib.h"

// stb_image 声明在 stb_image.h 中，实现集中在 core/assets/stb_image_impl.cpp
#include <stb_image.h>

namespace gryce_engine::assets {

namespace {

bool ends_with_ci(const std::string& str, const std::string& suffix) {
    if (str.size() < suffix.size()) return false;
    std::string a = str.substr(str.size() - suffix.size());
    std::string b = suffix;
    std::transform(a.begin(), a.end(), a.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(b.begin(), b.end(), b.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return a == b;
}

} // namespace

AssetManager& AssetManager::instance() {
    static AssetManager manager;
    return manager;
}

// ---------------------------------------------------------------------------
// 通用 load 模板定义
// ---------------------------------------------------------------------------
template<>
AssetHandle<MeshData> AssetManager::load<MeshData>(const std::string& path) {
    return AssetHandle<MeshData>(load_mesh_internal(path));
}

template<>
AssetHandle<TextureData> AssetManager::load<TextureData>(const std::string& path) {
    return AssetHandle<TextureData>(load_texture_internal(path));
}

// ---------------------------------------------------------------------------
// Mesh 加载
// ---------------------------------------------------------------------------
std::shared_ptr<MeshData> AssetManager::load_mesh_internal(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = assets_.find(path);
    if (it != assets_.end()) {
        return std::dynamic_pointer_cast<MeshData>(it->second);
    }

    std::string resolved = resources::ResourcePath::resolve(path);

    std::vector<MeshData> meshes;

    // OBJ 用内置加载器（快、UV 朝向可控）；其他格式走 Assimp
    if (ends_with_ci(resolved, ".obj")) {
        ObjLoader loader;
        meshes = loader.load(resolved);
    }
#ifdef GRYCE_HAS_ASSIMP
    else {
        AssimpImporter importer;
        meshes = importer.import(resolved);
    }
#else
    else {
        GLOG_ERROR("AssetManager: non-OBJ formats require Assimp ({})", path);
        return nullptr;
    }
#endif

    if (meshes.empty()) {
        GLOG_ERROR("AssetManager: failed to load mesh '{}'", path);
        return nullptr;
    }

    auto data = std::make_shared<MeshData>(std::move(meshes[0]));
    data->set_path(path);
    assets_[path] = data;
    GLOG_INFO("AssetManager: cached mesh '{}' ({} vertices)", path, data->vertices.size());
    return data;
}

// ---------------------------------------------------------------------------
// Texture 加载
// ---------------------------------------------------------------------------
std::shared_ptr<TextureData> AssetManager::load_texture_internal(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = assets_.find(path);
    if (it != assets_.end()) {
        return std::dynamic_pointer_cast<TextureData>(it->second);
    }

    std::string resolved = resources::ResourcePath::resolve(path);

    int width = 0, height = 0, channels = 0;
    unsigned char* data = stbi_load(resolved.c_str(), &width, &height, &channels, 0);
    if (!data) {
        GLOG_ERROR("AssetManager: failed to load texture '{}'", path);
        return nullptr;
    }

    auto tex = std::make_shared<TextureData>();
    tex->set_path(path);
    tex->width = width;
    tex->height = height;
    tex->channels = channels;
    tex->pixels.assign(data, data + static_cast<std::size_t>(width * height * channels));
    stbi_image_free(data);

    assets_[path] = tex;
    GLOG_INFO("AssetManager: cached texture '{}' ({}x{}, {} channels)", path, width, height, channels);
    return tex;
}

// ---------------------------------------------------------------------------
// 兼容 API
// ---------------------------------------------------------------------------
const MeshData* AssetManager::load_mesh(const std::string& path) {
    auto shared = load_mesh_internal(path);
    return shared.get();
}

std::shared_ptr<const MeshData> AssetManager::load_mesh_shared(const std::string& path) {
    return load_mesh_internal(path);
}

void AssetManager::unload(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    assets_.erase(path);
}

void AssetManager::unload_mesh(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    assets_.erase(path);
}

void AssetManager::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    assets_.clear();
}

bool AssetManager::has(const std::string& path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return assets_.find(path) != assets_.end();
}

bool AssetManager::has_mesh(const std::string& path) const {
    return has(path);
}

} // namespace gryce_engine::assets
