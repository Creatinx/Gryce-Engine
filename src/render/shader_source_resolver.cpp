#include "shader_source_resolver.h"

#include <fstream>
#include <sstream>
#include <filesystem>
#include <system_error>

#include "core_shaders.h"
#include "render.h"
#include "assets/asset_manager.h"
#include "resources/resource_path.h"

namespace gryce_engine::render {

namespace {

std::string read_file_text(const std::string& path) {
    if (path.empty()) return "";
    std::ifstream file(path);
    if (!file.is_open()) return "";
    std::stringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

std::string join_path(std::string dir, const std::string& file) {
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') dir += '/';
    return dir + file;
}

// 计算 shader_dir 相对 core 默认根（res:/shaders）的路径段，用于映射到
// 引擎默认 shader 目录下的同名文件。例如：
//   "res:/shaders"                 -> ""（根级）
//   "res:/shaders/forward_clustered" -> "forward_clustered"
std::string rel_to_core_root(const std::string& shader_dir) {
    const std::string prefix = core_shaders::kCoreShadersResRoot;
    if (!shader_dir.empty() && shader_dir.compare(0, prefix.size(), prefix) == 0) {
        std::string rel = shader_dir.substr(prefix.size());
        // 去掉开头的 '/' ，保留 forward_clustered 等子目录段
        while (!rel.empty() && (rel.front() == '/' || rel.front() == '\\')) rel.erase(rel.begin());
        return rel;
    }
    // 非默认根（绝对目录等）原样保留整段相对路径
    std::string rel = shader_dir;
    while (!rel.empty() && (rel.front() == '/' || rel.front() == '\\')) rel.erase(rel.begin());
    return rel;
}

// 尝试在"项目磁盘 → bundle"两级里定位 key 的可读路径。resolve_for_reading
// 本身会先查磁盘再查 bundle，这里直接用它即可；磁盘命中即时返回。
std::string resolve_from_disk_or_bundle(const std::string& key) {
    // 磁盘文件优先
    std::string resolved = resources::ResourcePath::resolve(key);
    std::error_code ec;
    if (std::filesystem::is_regular_file(resolved, ec)) {
        return resolved;
    }
    // 否则从已挂载 bundle 提取（含随项目打包的 core 兜底）
    return assets::AssetManager::instance().resolve_any(key);
}

// 解析单个扩展名（".vert" / ".frag"）。返回 readable 路径或空串。
// Vulkan 场景优先尝试 "vulkan_{name}"，缺失则回退 "{name}"。
std::string resolve_one(const std::string& name, const std::string& ext,
                        const std::string& shader_dir, RenderAPI api) {
    // 候选键名：Vulkan 优先 vulkan_ 前缀（descriptor binding 语义），否则直接基名。
    std::vector<std::string> keys;
    if (api == RenderAPI::Vulkan) {
        keys.push_back(join_path(shader_dir, "vulkan_" + name + ext));
    }
    keys.push_back(join_path(shader_dir, name + ext));

    for (const std::string& key : keys) {
        std::string p = resolve_from_disk_or_bundle(key);
        if (!p.empty()) return p;
    }

    // core 兜底：引擎默认 shader 目录（未打包/编辑器模式；打包产物已把 core
    // 兜底一并打进项目 shader 包，故此处仅在无盘文件缺失时补充定位）。
    const std::string engine_dir = core_shaders::engine_shaders_dir();
    if (!engine_dir.empty()) {
        const std::string rel = rel_to_core_root(shader_dir);
        const std::string relative =
            rel.empty() ? (name + ext) : (rel + "/" + name + ext);
        const std::string cand = join_path(engine_dir, relative);
        std::error_code ec;
        if (std::filesystem::is_regular_file(cand, ec)) return cand;
    }
    return "";
}

} // namespace

ShaderSourceSet resolve_shader_source(const std::string& name,
                                      const std::string& shader_dir,
                                      RenderAPI api) {
    ShaderSourceSet out;
    ShaderStageSource v = resolve_shader_stage_source(name, shader_dir, ".vert", api);
    ShaderStageSource f = resolve_shader_stage_source(name, shader_dir, ".frag", api);
    if (!v.ok() || !f.ok()) {
        return ShaderSourceSet{};
    }
    out.vertex = v.code;
    out.fragment = f.code;
    out.vertex_path = v.path;
    out.fragment_path = f.path;
    return out;
}

ShaderStageSource resolve_shader_stage_source(const std::string& name,
                                              const std::string& shader_dir,
                                              const char* ext,
                                              RenderAPI api) {
    ShaderStageSource out;
    const std::string path = resolve_one(name, ext, shader_dir, api);
    if (path.empty()) {
        return out;
    }
    out.code = read_file_text(path);
    out.path = path;
    return out;
}

} // namespace gryce_engine::render