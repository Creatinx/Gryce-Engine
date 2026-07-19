#include "asset_manager.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "assets/obj_loader.h"
#include "assets/compressed_image.h"
#include "assets/dds_loader.h"
#include "assets/ktx_loader.h"
#ifdef GRYCE_HAS_ASSIMP
#include "assets/assimp_importer.h"
#endif
#include "resources/resource_path.h"
#include "utils/glog/glog_lib.h"

// stb_image 声明在 stb_image.h 中，实现集中在 core/assets/stb_image_impl.cpp
#include <stb_image.h>
#include <tinyexr.h>

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
// SkinnedModelData 加载（骨骼 + 蒙皮 + 动画）
// ---------------------------------------------------------------------------
std::shared_ptr<SkinnedModelData> AssetManager::load_skinned_model_internal(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = skinned_models_.find(path);
    if (it != skinned_models_.end()) {
        return it->second;
    }

#ifdef GRYCE_HAS_ASSIMP
    const std::string resolved = resources::ResourcePath::resolve(path);
    AssimpImporter importer;
    auto model = std::make_shared<SkinnedModelData>(importer.import_skinned(resolved));
    if (model->meshes.empty()) {
        GLOG_ERROR("AssetManager: failed to load skinned model '{}'", path);
        return nullptr;
    }
    // 蒙皮骨骼数超 GPU palette 上限（render::k_max_skinning_bones = 128）时告警：
    // AnimatorSystem 求 palette 时会截断，超出的骨骼动画不生效。
    constexpr size_t k_gpu_bone_limit = 128;
    if (model->skeleton.bones.size() > k_gpu_bone_limit) {
        GLOG_WARN("AssetManager: '{}' has {} bones, exceeds GPU palette limit {}; truncated",
                  path, model->skeleton.bones.size(), k_gpu_bone_limit);
    }
    skinned_models_[path] = model;
    GLOG_INFO("AssetManager: cached skinned model '{}' ({} meshes, {} bones, {} clips, skin={})",
              path, model->meshes.size(), model->skeleton.bones.size(),
              model->animations.size(), model->has_skin);
    return model;
#else
    GLOG_ERROR("AssetManager: skinned models require Assimp ({})", path);
    return nullptr;
#endif
}

std::shared_ptr<SkinnedModelData> AssetManager::load_skinned_model(const std::string& path) {
    return load_skinned_model_internal(path);
}

void AssetManager::load_skinned_model_async(const std::string& path,
                                            std::function<void(std::shared_ptr<SkinnedModelData>)> on_complete) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = skinned_models_.find(path);
        if (it != skinned_models_.end()) {
            if (on_complete) on_complete(it->second);
            return;
        }
    }

    // import_skinned 为纯 CPU 工作，可直接在 AsyncLoader 工作线程执行；
    // GPU 上传由主线程/渲染线程在拿到数据后另行触发。
    auto* self = this;
    AsyncLoader::instance().submit(
        path, typeid(SkinnedModelData),
        [self, path]() { self->load_skinned_model_internal(path); },
        [self, path, on_complete]() {
            if (on_complete) on_complete(self->load_skinned_model(path));
        }
    );
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

    const std::string resolved = resources::ResourcePath::resolve(path);

    // --- 压缩纹理：DDS / KTX ---
    if (ends_with_ci(path, ".dds") || ends_with_ci(path, ".ktx")) {
        CompressedImage comp;
        if (!load_compressed_image(path, comp)) {
            GLOG_ERROR("AssetManager: failed to load compressed texture '{}'", path);
            return nullptr;
        }
        auto tex = std::make_shared<TextureData>();
        tex->set_path(path);
        tex->is_compressed = true;
        tex->width = comp.width;
        tex->height = comp.height;
        tex->mip_levels = comp.mip_levels;
        tex->compressed_format = comp.format;
        tex->mips = std::move(comp.mips);
        assets_[path] = tex;
        GLOG_INFO("AssetManager: cached compressed texture '{}' ({}x{}, mips={})",
                  path, tex->width, tex->height, tex->mip_levels);
        return tex;
    }

    // --- EXR ---
    if (ends_with_ci(path, ".exr")) {
        float* rgba = nullptr;
        int w = 0, h = 0;
        const char* err = nullptr;
        int ret = LoadEXR(&rgba, &w, &h, resolved.c_str(), &err);
        if (ret != TINYEXR_SUCCESS || !rgba) {
            GLOG_ERROR("AssetManager: failed to load EXR '{}': {}", path, err ? err : "unknown");
            FreeEXRErrorMessage(err);
            return nullptr;
        }
        auto tex = std::make_shared<TextureData>();
        tex->set_path(path);
        tex->is_float = true;
        tex->width = w;
        tex->height = h;
        tex->channels = 4;
        tex->float_pixels.assign(rgba, rgba + static_cast<size_t>(w) * h * 4);
        std::free(rgba);
        assets_[path] = tex;
        GLOG_INFO("AssetManager: cached EXR '{}' ({}x{}, RGBA32F)", path, w, h);
        return tex;
    }

    // --- HDR / LDR 通用 ---
    const bool is_hdr = stbi_is_hdr(resolved.c_str());
    int width = 0, height = 0, channels = 0;

    if (is_hdr) {
        float* data = stbi_loadf(resolved.c_str(), &width, &height, &channels, 4);
        if (!data) {
            GLOG_ERROR("AssetManager: failed to load HDR texture '{}'", path);
            return nullptr;
        }
        auto tex = std::make_shared<TextureData>();
        tex->set_path(path);
        tex->is_float = true;
        tex->width = width;
        tex->height = height;
        tex->channels = 4;
        tex->float_pixels.assign(data, data + static_cast<size_t>(width) * height * 4);
        stbi_image_free(data);
        assets_[path] = tex;
        GLOG_INFO("AssetManager: cached HDR texture '{}' ({}x{}, RGBA32F)", path, width, height);
        return tex;
    }

    // LDR 8-bit
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
    skinned_models_.clear();
}

bool AssetManager::has(const std::string& path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return assets_.find(path) != assets_.end();
}

bool AssetManager::has_mesh(const std::string& path) const {
    return has(path);
}

// ---------------------------------------------------------------------------
// 异步加载特化
// ---------------------------------------------------------------------------
template<>
void AssetManager::load_async<MeshData>(const std::string& path,
                                        std::function<void(AssetHandle<MeshData>)> on_complete) {
    // 已缓存则同步回调
    if (has(path)) {
        if (on_complete) on_complete(load<MeshData>(path));
        return;
    }

    auto* self = this;
    AsyncLoader::instance().submit(
        path, typeid(MeshData),
        [self, path]() { self->load_mesh_internal(path); },
        [self, path, on_complete]() {
            if (on_complete) on_complete(self->load<MeshData>(path));
        }
    );
}

template<>
void AssetManager::load_async<TextureData>(const std::string& path,
                                           std::function<void(AssetHandle<TextureData>)> on_complete) {
    if (has(path)) {
        if (on_complete) on_complete(load<TextureData>(path));
        return;
    }

    auto* self = this;
    AsyncLoader::instance().submit(
        path, typeid(TextureData),
        [self, path]() { self->load_texture_internal(path); },
        [self, path, on_complete]() {
            if (on_complete) on_complete(self->load<TextureData>(path));
        }
    );
}

// ---------------------------------------------------------------------------
// 异步加载状态查询
// ---------------------------------------------------------------------------
LoadingState AssetManager::get_async_state(const std::string& path) const {
    return AsyncLoader::instance().get_state(path);
}

bool AssetManager::is_async_loading(const std::string& path) const {
    return AsyncLoader::instance().is_loading(path);
}

} // namespace gryce_engine::assets
