#include "assimp_importer.h"

#include <assimp/Importer.hpp>
#include <assimp/config.h>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/texture.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <stb_image.h>
#include <stb_image_write.h>

#include "utils/crypto/crc32.h"
#include "utils/glog/glog_lib.h"

#include "assets/texture_path.h"

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

// 贴图路径工具（反斜杠归一化 / 绝对路径 / 大小写不敏感查找）与 OBJ 加载器共用
using texture_path::normalize_separators;

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
// cache 以贴图索引为键去重：同一 scene 内多个材质引用同一内嵌贴图只解析一次。
//
// 落盘文件名带**内容哈希**（crc32）而不是只带索引：不同模型的内嵌贴图都是从
// "*0" 开始的，只用索引命名会让第二个模型拿到第一个模型的图（缓存串味）。
std::string extract_embedded_texture(const aiScene* scene, unsigned int index,
                                     std::unordered_map<unsigned int, std::string>& cache) {
    if (index >= scene->mNumTextures) return {};

    auto it = cache.find(index);
    if (it != cache.end()) return it->second;

    const aiTexture* tex = scene->mTextures[index];
    if (!tex || !tex->pcData) return {};

    std::error_code ec;
    fs::create_directories(embedded_texture_cache_dir(), ec); // 已存在时静默

    const uint8_t* hash_data = nullptr;
    size_t hash_size = 0;

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

        hash_data = data;
        hash_size = size;
        char name_buf[64];
        std::snprintf(name_buf, sizeof(name_buf), "embedded_%08x.%s",
                      utils::crc32(data, size), ext.c_str());
        path = embedded_texture_cache_dir() / name_buf;

        if (!fs::is_regular_file(path, ec)) {
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
        }
    } else {
        // 非压缩 ARGB8888 像素数组
        const uint64_t pixel_bytes = static_cast<uint64_t>(tex->mWidth) * tex->mHeight * 4ull;
        hash_data = reinterpret_cast<const uint8_t*>(tex->pcData);
        hash_size = static_cast<size_t>(pixel_bytes);

        char name_buf[64];
        std::snprintf(name_buf, sizeof(name_buf), "embedded_%08x.tga",
                      utils::crc32(hash_data, hash_size));
        path = embedded_texture_cache_dir() / name_buf;
        if (!fs::is_regular_file(path, ec) && !write_uncompressed_tga(path, tex)) {
            GLOG_ERROR("AssimpImporter: failed decoding embedded texture '{}'", path.string());
            return {};
        }
    }

    cache.emplace(index, path.string());
    return path.string();
}

// ---------------------------------------------------------------------------
// 打包贴图拆通道（glTF metallicRoughness / 常见 ORM 布局）
// ---------------------------------------------------------------------------
// glTF 把三张灰度图打包进一张贴图：R=AO、G=roughness、B=metallic。
// 引擎的 roughness / metallic / ao 三个槽位各自采样 .r，共用同一张图会让三者
// 全读到 R 通道 —— 表现就是"文件里明明写了粗糙度，模型却要么全金属要么全粗糙"。
// 导入时拆成单通道 PNG 缓存起来，渲染侧完全不用改。
fs::path derived_map_cache_dir() {
    static const fs::path dir = fs::temp_directory_path() / "gryce-derived-maps";
    return dir;
}

std::string write_channel_png(const std::vector<uint8_t>& rgba, int width, int height,
                              int channel, uint32_t content_hash, const char* tag) {
    std::error_code ec;
    fs::create_directories(derived_map_cache_dir(), ec);

    char name_buf[96];
    std::snprintf(name_buf, sizeof(name_buf), "%08x_%s.png", content_hash, tag);
    const fs::path path = derived_map_cache_dir() / name_buf;
    if (fs::is_regular_file(path, ec)) return normalize_separators(path.string());

    const size_t pixel_count = static_cast<size_t>(width) * static_cast<size_t>(height);
    std::vector<uint8_t> plane(pixel_count);
    for (size_t i = 0; i < pixel_count; ++i) {
        plane[i] = rgba[i * 4 + static_cast<size_t>(channel)];
    }
    if (!stbi_write_png(path.string().c_str(), width, height, 1, plane.data(), width)) {
        GLOG_WARN("AssimpImporter: failed writing derived map '{}'", path.string());
        return {};
    }
    return normalize_separators(path.string());
}

// 把一张打包贴图拆成 roughness(G)/metallic(B)/ao(R) 三张单通道图。
bool split_packed_channels(const std::string& path, std::string* roughness_out,
                           std::string* metallic_out, std::string* ao_out) {
    int w = 0;
    int h = 0;
    int ch = 0;
    stbi_uc* pixels = stbi_load(path.c_str(), &w, &h, &ch, 4);
    if (!pixels) {
        GLOG_WARN("AssimpImporter: cannot decode packed map '{}', channels kept as-is", path);
        return false;
    }
    const size_t byte_count = static_cast<size_t>(w) * static_cast<size_t>(h) * 4ull;
    const std::vector<uint8_t> rgba(pixels, pixels + byte_count);
    stbi_image_free(pixels);

    const uint32_t hash = utils::crc32(rgba.data(), rgba.size());
    if (roughness_out) *roughness_out = write_channel_png(rgba, w, h, 1, hash, "roughness");
    if (metallic_out) *metallic_out = write_channel_png(rgba, w, h, 2, hash, "metallic");
    if (ao_out) *ao_out = write_channel_png(rgba, w, h, 0, hash, "ao");
    return true;
}

const aiMaterialProperty* find_property(const aiMaterial* mat, const char* key) {
    for (unsigned i = 0; i < mat->mNumProperties; ++i) {
        const aiMaterialProperty* prop = mat->mProperties[i];
        if (prop && std::strcmp(prop->mKey.C_Str(), key) == 0) return prop;
    }
    return nullptr;
}

// 从 aiMaterial 提取材质数据。
//
// 各格式能提供的信息差别很大，这里只负责"把文件里有的都读出来"，
// 真正的 PBR 推导统一交给 material_rules.h::resolve_surface()：
//   金属度工作流（glTF / 新版 FBX）→ metallicFactor / roughnessFactor 直接可用
//   镜面工作流（老 FBX / Collada）→ Ks / Shininess 反推
//   什么都没有（PLY / STL）→ 按有无基色贴图兜底
void process_material(const aiScene* scene, aiMesh* mesh, const std::string& base_dir,
                      std::unordered_map<unsigned int, std::string>& embedded_cache,
                      MeshData& data) {
    if (!scene->HasMaterials() || mesh->mMaterialIndex >= scene->mNumMaterials) return;
    const aiMaterial* mat = scene->mMaterials[mesh->mMaterialIndex];

    SurfaceInput input;
    aiString material_name;
    if (mat->Get(AI_MATKEY_NAME, material_name) == AI_SUCCESS) {
        data.material.name = material_name.C_Str();
    }

    // ---- 贴图：按槽位优先级取第一张能真正落盘的 ----
    auto get_texture = [&](std::initializer_list<aiTextureType> types) -> std::string {
        for (aiTextureType type : types) {
            if (mat->GetTextureCount(type) == 0) continue;
            aiString tex_path;
            if (mat->GetTexture(type, 0, &tex_path) != AI_SUCCESS) continue;
            const char* raw = tex_path.C_Str();
            if (!raw || raw[0] == '\0') continue;

            if (raw[0] == '*') {
                // 内嵌贴图引用："*N" → aiScene::mTextures[N]
                const unsigned int index = static_cast<unsigned int>(std::strtoul(raw + 1, nullptr, 10));
                const std::string extracted = extract_embedded_texture(scene, index, embedded_cache);
                if (!extracted.empty()) return extracted;
                continue;
            }
            const std::string resolved = texture_path::resolve(base_dir, raw);
            if (!resolved.empty()) return resolved;
            GLOG_WARN("AssimpImporter: material '{}' references missing texture '{}' (searched '{}')",
                      data.material.name, raw, base_dir);
        }
        return {};
    };

    data.material.albedo_map = get_texture({aiTextureType_BASE_COLOR, aiTextureType_DIFFUSE});
    data.material.normal_map = get_texture({aiTextureType_NORMAL_CAMERA, aiTextureType_NORMALS});
    data.material.roughness_map = get_texture({aiTextureType_DIFFUSE_ROUGHNESS});
    data.material.metallic_map = get_texture({aiTextureType_METALNESS});
    data.material.ao_map = get_texture({aiTextureType_AMBIENT_OCCLUSION, aiTextureType_LIGHTMAP});
    data.material.emissive_map = get_texture({aiTextureType_EMISSIVE, aiTextureType_EMISSION_COLOR});
    const std::string specular_map = get_texture({aiTextureType_SPECULAR});

    // 打包贴图（roughness 与 metallic 指向同一张 ORM）→ 按通道拆开
    if (!data.material.roughness_map.empty() &&
        data.material.roughness_map == data.material.metallic_map) {
        const bool ao_packed = (data.material.ao_map == data.material.roughness_map);
        GLOG_INFO("AssimpImporter: material '{}' uses a packed metallic/roughness map, splitting channels",
                  data.material.name);
        split_packed_channels(data.material.roughness_map, &data.material.roughness_map,
                              &data.material.metallic_map,
                              ao_packed ? &data.material.ao_map : nullptr);
    }

    // ---- 标量参数 ----
    aiColor3D color;
    if (mat->Get(AI_MATKEY_COLOR_DIFFUSE, color) == AI_SUCCESS) {
        input.base_color = math::Vector3f(color.r, color.g, color.b);
    }
    if (mat->Get(AI_MATKEY_COLOR_SPECULAR, color) == AI_SUCCESS) {
        input.specular_color = math::Vector3f(color.r, color.g, color.b);
        // assimp 的 FBX 导入器对 phong 材质即使文件没写 specular 也会填 0.9 灰，
        // 真值只在 "$raw.Specular" 里 —— 拿默认值当"作者写了镜面"会把大量
        // 电介质误判成金属。其它格式没有 $raw.* 时，0.9 灰且无镜面贴图也算未指定。
        const aiMaterialProperty* raw_spec = find_property(mat, "$raw.Specular");
        if (raw_spec) {
            float raw_values[3] = {0.0f, 0.0f, 0.0f};
            if (raw_spec->mType == aiPTI_Float &&
                raw_spec->mDataLength >= sizeof(raw_values)) {
                std::memcpy(raw_values, raw_spec->mData, sizeof(raw_values));
            }
            input.has_specular = (raw_values[0] + raw_values[1] + raw_values[2]) > 1e-4f;
        } else {
            const bool default_gray = std::abs(color.r - 0.9f) < 1e-3f &&
                                      std::abs(color.g - 0.9f) < 1e-3f &&
                                      std::abs(color.b - 0.9f) < 1e-3f;
            input.has_specular = !default_gray || !specular_map.empty();
        }
    }
    if (mat->Get(AI_MATKEY_COLOR_EMISSIVE, color) == AI_SUCCESS) {
        input.emissive = math::Vector3f(color.r, color.g, color.b);
    }

    float metallic_factor = 0.0f;
    if (mat->Get(AI_MATKEY_METALLIC_FACTOR, metallic_factor) == AI_SUCCESS) {
        input.has_metallic = true;
        input.metallic = metallic_factor;
    }
    float roughness_factor = -1.0f;
    if (mat->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughness_factor) == AI_SUCCESS &&
        roughness_factor >= 0.0f) {
        // FBX 的 Physical Material、glTF 的 roughnessFactor 都落在这里；
        // 优先级必须高于 Shininess —— 老 FBX 常同时带 shininess=2（3ds Max 默认）
        // 和一条真正的 roughnessFactor，用 shininess 反推会让整模型比文件写的更亮。
        input.has_roughness = true;
        input.roughness = roughness_factor;
    }
    float shininess = -1.0f;
    if (mat->Get(AI_MATKEY_SHININESS, shininess) == AI_SUCCESS && shininess >= 0.0f) {
        input.has_shininess = true;
        input.shininess = shininess;
    }
    float specular_strength = 1.0f;
    if (mat->Get(AI_MATKEY_SPECULAR_FACTOR, specular_strength) == AI_SUCCESS) {
        input.has_specular_strength = true;
        input.specular_strength = specular_strength;
    }

    float opacity = 1.0f;
    if (mat->Get(AI_MATKEY_OPACITY, opacity) == AI_SUCCESS) {
        input.has_opacity = true;
        input.opacity = opacity;
    } else {
        // 部分格式只给透明度因子：opacity = 1 - transparency
        float transparency = 0.0f;
        if (mat->Get(AI_MATKEY_TRANSPARENCYFACTOR, transparency) == AI_SUCCESS &&
            transparency > 0.0f) {
            input.has_opacity = true;
            input.opacity = 1.0f - transparency;
        }
    }
    int two_sided = 0;
    if (mat->Get(AI_MATKEY_TWOSIDED, two_sided) == AI_SUCCESS && two_sided != 0) {
        input.two_sided = true;
    }

    const bool has_diffuse_map = !data.material.albedo_map.empty();
    input.has_base_color_map = has_diffuse_map;
    input.has_specular_map = !specular_map.empty();

    // 有自发光贴图但没写自发光颜色：颜色按白色处理，否则 shader 里
    // emissive = uEmissiveColor * map 恒为 0，贴图等于白导。
    if (!data.material.emissive_map.empty() &&
        surface_luminance(input.emissive) < 1e-4f) {
        input.emissive = math::Vector3f::one();
    }

    const SurfaceResult surface = resolve_surface(input);

    data.material.albedo_color = surface.base_color;
    data.material.roughness = surface.roughness;
    data.material.metallic = surface.metallic;
    data.material.opacity = surface.opacity;
    data.material.emissive_color = surface.emissive;
    data.material.two_sided = surface.two_sided;
    data.material.surface_type = surface.type;

    // 镜面工作流里的金属：没有漫反射贴图时，镜面贴图就是它的基色贴图
    if (!has_diffuse_map && surface.metallic > 0.5f && !specular_map.empty()) {
        data.material.albedo_map = specular_map;
    }

    data.material.valid = true;
    GLOG_INFO("AssimpImporter: material '{}' -> {} (roughness {}, metallic {}, opacity {}, maps: albedo={}, normal={}, rough={}, metal={}, ao={}, emissive={})",
              data.material.name, surface_type_name(surface.type), surface.roughness,
              surface.metallic, surface.opacity,
              !data.material.albedo_map.empty(), !data.material.normal_map.empty(),
              !data.material.roughness_map.empty(), !data.material.metallic_map.empty(),
              !data.material.ao_map.empty(), !data.material.emissive_map.empty());
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

// 统一的读取配置（含 post-process 标志）。
// FBX 内嵌贴图（Video 的 Content 数据）默认以外部文件名引用（assimp 的
// useLegacyEmbeddedTextureNaming 默认为 false），而这些名字通常是导出机的
// 临时路径、换机后必然失效 → 开启 legacy 命名让内嵌贴图按 "*N" 引用，
// 交由 extract_embedded_texture 提取落盘。对未内嵌贴图的模型无影响。
const aiScene* read_scene(Assimp::Importer& importer, const std::string& path) {
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_EMBEDDED_TEXTURES_LEGACY_NAMING, true);
    return importer.ReadFile(path,
        aiProcess_Triangulate |
        aiProcess_FlipUVs |
        aiProcess_GenNormals |
        aiProcess_CalcTangentSpace);
}

} // namespace

std::vector<MeshData> AssimpImporter::import(const std::string& path) const {
    std::vector<MeshData> result;

    Assimp::Importer importer;
    const aiScene* scene = read_scene(importer, path);

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
    // 与 import() 相同的读取配置（post-process 标志 + 内嵌贴图 legacy 命名）；
    // 蒙皮权重随顶点保留，不受影响
    const aiScene* scene = read_scene(importer, path);

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
