#include "asset_manager.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>

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
    auto shared = load_mesh_internal(path);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        touch_unlocked(path);
        maybe_evict_unlocked();
    }
    return AssetHandle<MeshData>(std::move(shared));
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
        touch_unlocked(path);
        return std::dynamic_pointer_cast<MeshData>(it->second.asset);
    }

    std::string resolved = resources::ResourcePath::resolve(path);
    if (!std::filesystem::exists(resolved)) {
        std::string temp_path = extract_from_bundle_unlocked(path);
        if (!temp_path.empty()) {
            resolved = std::move(temp_path);
        }
    }

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
    CacheEntry entry;
    entry.asset = data;
    entry.memory_size = data->memory_size();
    entry.last_access = std::chrono::steady_clock::now();
    assets_[path] = std::move(entry);
    GLOG_INFO("AssetManager: cached mesh '{}' ({} vertices)", path, data->vertices.size());
    return data;
}

// ---------------------------------------------------------------------------
// Texture 加载
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
        touch_unlocked(path);
        return std::dynamic_pointer_cast<TextureData>(it->second.asset);
    }

    std::string resolved = resources::ResourcePath::resolve(path);
    if (!std::filesystem::exists(resolved)) {
        std::string temp_path = extract_from_bundle_unlocked(path);
        if (!temp_path.empty()) {
            resolved = std::move(temp_path);
        }
    }

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
            CacheEntry entry;
    entry.asset = tex;
    entry.memory_size = tex->memory_size();
    entry.last_access = std::chrono::steady_clock::now();
    assets_[path] = std::move(entry);
    maybe_evict_unlocked();
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
            CacheEntry entry;
    entry.asset = tex;
    entry.memory_size = tex->memory_size();
    entry.last_access = std::chrono::steady_clock::now();
    assets_[path] = std::move(entry);
    maybe_evict_unlocked();
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
            CacheEntry entry;
    entry.asset = tex;
    entry.memory_size = tex->memory_size();
    entry.last_access = std::chrono::steady_clock::now();
    assets_[path] = std::move(entry);
    maybe_evict_unlocked();
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

        CacheEntry entry;
    entry.asset = tex;
    entry.memory_size = tex->memory_size();
    entry.last_access = std::chrono::steady_clock::now();
    assets_[path] = std::move(entry);
    maybe_evict_unlocked();
    GLOG_INFO("AssetManager: cached texture '{}' ({}x{}, {} channels)", path, width, height, channels);
    return tex;
}

// ---------------------------------------------------------------------------
// 兼容 API
// ---------------------------------------------------------------------------
const MeshData* AssetManager::load_mesh(const std::string& path) {
    auto shared = load_mesh_internal(path);
    const MeshData* raw = shared ? shared.get() : nullptr;
    shared.reset();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        touch_unlocked(path);
        maybe_evict_unlocked();
    }
    return raw;
}

std::shared_ptr<const MeshData> AssetManager::load_mesh_shared(const std::string& path) {
    auto shared = load_mesh_internal(path);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        touch_unlocked(path);
        maybe_evict_unlocked();
    }
    return shared;
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

void AssetManager::set_max_cache_count(size_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    max_count_ = count;
    maybe_evict_unlocked();
}

void AssetManager::set_max_cache_memory_mb(float mb) {
    std::lock_guard<std::mutex> lock(mutex_);
    max_memory_bytes_ = static_cast<size_t>(mb * 1024.0 * 1024.0);
    maybe_evict_unlocked();
}

size_t AssetManager::max_cache_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return max_count_;
}

float AssetManager::max_cache_memory_mb() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<float>(max_memory_bytes_) / (1024.0f * 1024.0f);
}

size_t AssetManager::resident_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return assets_.size();
}

size_t AssetManager::resident_memory_bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t total = 0;
    for (const auto& [path, entry] : assets_) {
        (void)path;
        total += entry.memory_size;
    }
    return total;
}

void AssetManager::touch_unlocked(const std::string& path) {
    auto it = assets_.find(path);
    if (it != assets_.end()) {
        it->second.last_access = std::chrono::steady_clock::now();
    }
}

void AssetManager::maybe_evict_unlocked() {
    // 无限制时不驱逐
    if (max_count_ == 0 && max_memory_bytes_ == 0) return;

    while (!assets_.empty()) {
        const bool count_overflow = (max_count_ > 0 && assets_.size() > max_count_);
        size_t total_memory = 0;
        for (const auto& [p, e] : assets_) {
            (void)p;
            total_memory += e.memory_size;
        }
        const bool memory_overflow = (max_memory_bytes_ > 0 && total_memory > max_memory_bytes_);

        if (!count_overflow && !memory_overflow) break;

        // 找最久未访问的条目
        auto oldest = assets_.end();
        for (auto it = assets_.begin(); it != assets_.end(); ++it) {
            if (oldest == assets_.end() ||
                it->second.last_access < oldest->second.last_access) {
                oldest = it;
            }
        }
        if (oldest == assets_.end()) break;

        // 只有最久未访问且仅被缓存持有的条目才会被驱逐；
        // 若最久条目仍被外部引用，则宁可暂时超出限制也不驱逐其他较新资源。
        if (oldest->second.asset.use_count() > 1) break;

        GLOG_INFO("AssetManager: evicted '{}' from cache ({} bytes)",
                  oldest->first, oldest->second.memory_size);
        assets_.erase(oldest);
    }
}

int AssetManager::mount_bundle(const std::string& pack_path) {
    auto reader = std::make_unique<resources::GPackReader>();
    if (!reader->open(pack_path)) return -1;

    std::lock_guard<std::mutex> lock(mutex_);
    int id = next_bundle_id_++;
    MountedBundle bundle;
    bundle.id = id;
    bundle.reader = std::move(reader);
    bundles_[id] = std::move(bundle);
    GLOG_INFO("AssetManager: mounted bundle '{}' (id={})", pack_path, id);
    return id;
}

void AssetManager::unmount_bundle(int id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = bundles_.find(id);
    if (it == bundles_.end()) return;

    std::error_code ec;
    for (const auto& [internal_path, temp_path] : it->second.extracted_temp_paths) {
        (void)internal_path;
        std::filesystem::remove(temp_path, ec);
    }
    bundles_.erase(it);
}

std::string AssetManager::extract_from_bundle_unlocked(const std::string& path) {
    std::string internal_path = path;
    if (internal_path.rfind("res:/", 0) == 0) {
        internal_path = internal_path.substr(5);
    }
    if (internal_path.empty()) return "";

    for (auto& [id, bundle] : bundles_) {
        (void)id;
        if (!bundle.reader->contains(internal_path)) continue;

        auto it = bundle.extracted_temp_paths.find(internal_path);
        if (it != bundle.extracted_temp_paths.end()) {
            return it->second;
        }

        auto data = bundle.reader->read(internal_path);
        if (data.empty()) continue;

        std::string filename = std::filesystem::path(internal_path).filename().string();
        if (filename.empty()) filename = "bundle_data";
        std::filesystem::path temp_dir = std::filesystem::temp_directory_path() / "gryce_bundle";
        std::error_code ec;
        std::filesystem::create_directories(temp_dir, ec);
        std::string temp_path = (temp_dir / (std::to_string(bundle.id) + "_" + filename)).string();

        std::ofstream ofs(temp_path, std::ios::binary);
        if (!ofs) continue;
        ofs.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
        if (!ofs.good()) continue;

        bundle.extracted_temp_paths[internal_path] = temp_path;
        return temp_path;
    }
    return "";
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
