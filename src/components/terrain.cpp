#include "components/terrain.h"

#include <algorithm>
#include <cmath>
#include <random>

#include "utils/glog/glog_lib.h"

namespace gryce_engine::components {

namespace {

constexpr float k_pi = 3.14159265358979323846f;

// 简单的哈希噪声 [-1, 1]
float hash_noise(int x, int z, int seed) {
    int n = x * 374761393 + z * 668265263 + seed * 1013904223;
    n = (n ^ (n >> 13)) * 1274126177;
    n = n ^ (n >> 16);
    return static_cast<float>(n & 0x7fffffff) / static_cast<float>(0x7fffffff) * 2.0f - 1.0f;
}

// 平滑插值
float smooth(float t) { return t * t * (3.0f - 2.0f * t); }

float value_noise(float x, float z, int seed) {
    int ix = static_cast<int>(std::floor(x));
    int iz = static_cast<int>(std::floor(z));
    float fx = x - static_cast<float>(ix);
    float fz = z - static_cast<float>(iz);

    float v00 = hash_noise(ix, iz, seed);
    float v10 = hash_noise(ix + 1, iz, seed);
    float v01 = hash_noise(ix, iz + 1, seed);
    float v11 = hash_noise(ix + 1, iz + 1, seed);

    float sx = smooth(fx);
    float sz = smooth(fz);

    float v0 = v00 + (v10 - v00) * sx;
    float v1 = v01 + (v11 - v01) * sx;
    return v0 + (v1 - v0) * sz;
}

} // namespace

void Terrain::serialize(nlohmann::json& out) const {
    out["width"] = width;
    out["depth"] = depth;
    out["resolution"] = resolution;
    out["height_scale"] = height_scale;
    out["base_texture_path"] = base_texture_path;
    out["seed"] = seed;
    out["heightmap"] = heightmap_;
}

void Terrain::deserialize(const nlohmann::json& in) {
    width = in.value("width", 100.0f);
    depth = in.value("depth", 100.0f);
    resolution = in.value("resolution", 64);
    height_scale = in.value("height_scale", 10.0f);
    base_texture_path = in.value("base_texture_path", std::string());
    seed = in.value("seed", 0);

    const int expected = heightmap_size();
    if (in.contains("heightmap") && in["heightmap"].is_array()) {
        heightmap_.clear();
        for (const auto& v : in["heightmap"]) {
            heightmap_.push_back(v.get<float>());
        }
    }
    if (static_cast<int>(heightmap_.size()) != expected) {
        ensure_heightmap();
        generate_noise();
    }
}

int Terrain::heightmap_size() const {
    return std::max(2, resolution + 1);
}

float Terrain::height_at(int x, int z) const {
    const int s = heightmap_size();
    x = std::clamp(x, 0, s - 1);
    z = std::clamp(z, 0, s - 1);
    return heightmap_[z * s + x];
}

void Terrain::set_height(int x, int z, float h) {
    const int s = heightmap_size();
    if (x < 0 || x >= s || z < 0 || z >= s) return;
    heightmap_[z * s + x] = std::clamp(h, 0.0f, 1.0f);
}

void Terrain::ensure_heightmap() {
    const int s = heightmap_size();
    heightmap_.assign(s * s, 0.0f);
}

void Terrain::normalize_heights() {
    if (heightmap_.empty()) return;
    float lo = heightmap_[0];
    float hi = heightmap_[0];
    for (float h : heightmap_) {
        lo = std::min(lo, h);
        hi = std::max(hi, h);
    }
    if (hi - lo < 1e-6f) return;
    const float inv = 1.0f / (hi - lo);
    for (float& h : heightmap_) {
        h = (h - lo) * inv;
    }
}

void Terrain::generate_noise() {
    ensure_heightmap();
    const int s = heightmap_size();
    const float scale = 0.05f;

    for (int z = 0; z < s; ++z) {
        for (int x = 0; x < s; ++x) {
            float fx = static_cast<float>(x) * scale;
            float fz = static_cast<float>(z) * scale;
            float h = value_noise(fx, fz, seed) * 0.5f + 0.5f;
            h += value_noise(fx * 2.0f, fz * 2.0f, seed + 1) * 0.25f;
            h += value_noise(fx * 4.0f, fz * 4.0f, seed + 2) * 0.125f;
            heightmap_[z * s + x] = std::clamp(h, 0.0f, 1.0f);
        }
    }
    normalize_heights();
}

math::Vector3f Terrain::compute_normal(int x, int z) const {
    const float dx = width / static_cast<float>(resolution);
    const float dz = depth / static_cast<float>(resolution);

    float hL = height_at(x - 1, z) * height_scale;
    float hR = height_at(x + 1, z) * height_scale;
    float hD = height_at(x, z - 1) * height_scale;
    float hU = height_at(x, z + 1) * height_scale;

    math::Vector3f normal(hL - hR, 2.0f * dx, hD - hU);
    return normal.normalized();
}

assets::MeshData Terrain::build_mesh_data() const {
    assets::MeshData data;
    const int s = heightmap_size();
    const float half_w = width * 0.5f;
    const float half_d = depth * 0.5f;
    const float dx = width / static_cast<float>(resolution);
    const float dz = depth / static_cast<float>(resolution);

    data.vertices.reserve(s * s);
    for (int z = 0; z < s; ++z) {
        for (int x = 0; x < s; ++x) {
            assets::MeshVertex v;
            v.position = math::Vector3f(
                -half_w + static_cast<float>(x) * dx,
                height_at(x, z) * height_scale,
                -half_d + static_cast<float>(z) * dz
            );
            v.normal = compute_normal(x, z);
            v.tangent = math::Vector3f::right();
            v.uv = math::Vector2f(
                static_cast<float>(x) / static_cast<float>(resolution),
                static_cast<float>(z) / static_cast<float>(resolution)
            );
            v.color = math::Vector3f::one();
            data.vertices.push_back(v);
        }
    }

    data.indices.reserve(resolution * resolution * 6);
    for (int z = 0; z < resolution; ++z) {
        for (int x = 0; x < resolution; ++x) {
            uint32_t i0 = z * s + x;
            uint32_t i1 = z * s + (x + 1);
            uint32_t i2 = (z + 1) * s + x;
            uint32_t i3 = (z + 1) * s + (x + 1);

            data.indices.push_back(i0);
            data.indices.push_back(i2);
            data.indices.push_back(i1);

            data.indices.push_back(i1);
            data.indices.push_back(i2);
            data.indices.push_back(i3);
        }
    }

    data.name = "TerrainMesh";
    return data;
}

} // namespace gryce_engine::components
