#include "core/meta.h"

#include <fstream>
#include <sstream>

#include "core/fsutil.h"
#include "core/pe_image.h"

namespace grycegc {

namespace {

constexpr const char* kMetaFileName = "gryce.meta.json";
constexpr const char* kProjectDataName = "project.data";

} // namespace

MetaDoc MetaDoc::from_json(const nlohmann::json& j) {
    MetaDoc d;
    if (j.is_object()) {
        if (j.contains("name") && j["name"].is_string()) d.name = j["name"].get<std::string>();
        if (j.contains("version") && j["version"].is_string())
            d.version = j["version"].get<std::string>();
        if (j.contains("copyright") && j["copyright"].is_string())
            d.copyright = j["copyright"].get<std::string>();
        if (j.contains("license") && j["license"].is_string())
            d.license = j["license"].get<std::string>();
        if (j.contains("build_id") && j["build_id"].is_string())
            d.build_id = j["build_id"].get<std::string>();
        if (j.contains("built_at") && j["built_at"].is_string())
            d.built_at = j["built_at"].get<std::string>();
        if (j.contains("toolchain") && j["toolchain"].is_string())
            d.toolchain = j["toolchain"].get<std::string>();
        if (j.contains("tags") && j["tags"].is_array()) {
            for (const auto& t : j["tags"])
                if (t.is_string()) d.tags.push_back(t.get<std::string>());
        }
        d.extra = j;  // 保留全量，to_json 时优先用结构化字段
    }
    return d;
}

nlohmann::json MetaDoc::to_json() const {
    nlohmann::json j = nlohmann::json::object();
    if (!extra.is_null() && extra.is_object()) j = extra;  // 以既有键为底
    if (!name.empty()) j["name"] = name;
    if (!version.empty()) j["version"] = version;
    if (!copyright.empty()) j["copyright"] = copyright;
    if (!license.empty()) j["license"] = license;
    if (!build_id.empty()) j["build_id"] = build_id;
    if (!built_at.empty()) j["built_at"] = built_at;
    if (!toolchain.empty()) j["toolchain"] = toolchain;
    if (!tags.empty()) j["tags"] = tags;
    return j;
}

MetaDoc meta_read_dir(const fs::path& dir) {
    const fs::path p = dir / kMetaFileName;
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    try {
        return MetaDoc::from_json(nlohmann::json::parse(ss.str()));
    } catch (const nlohmann::json::exception&) {
        return {};  // 损坏的 JSON 视为空（调用方会覆盖写）
    }
}

bool meta_write_dir(const fs::path& dir, const MetaDoc& doc) {
    mkdir_p(dir);
    const std::string s = doc.to_json().dump(2) + "\n";
    return write_binary(dir / kMetaFileName, s);
}

bool meta_merge_project_data(const fs::path& project_dir,
                             const std::vector<std::pair<std::string, std::string>>& kv) {
    const fs::path p = project_dir / kProjectDataName;
    std::vector<std::string> lines;
    {
        std::ifstream f(p, std::ios::binary);
        if (f) {
            std::string line;
            while (std::getline(f, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                lines.push_back(line);
            }
        }
    }
    for (const auto& [key, value] : kv) {
        const std::string prefix = key + "=";
        bool replaced = false;
        for (auto& line : lines) {
            if (line.rfind(prefix, 0) == 0) {  // 注释行以 # 开头不会匹配
                line = prefix + value;
                replaced = true;
                break;
            }
        }
        if (!replaced) lines.push_back(prefix + value);
    }
    std::string out;
    for (const auto& line : lines) out += line + "\n";
    return write_binary(p, out);
}

std::optional<std::string> project_data_get(const fs::path& project_dir,
                                            const std::string& key) {
    std::ifstream f(project_dir / kProjectDataName, std::ios::binary);
    if (!f) return std::nullopt;
    const std::string prefix = key + "=";
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind(prefix, 0) == 0) return line.substr(prefix.size());
    }
    return std::nullopt;
}

bool meta_write_pe_trailer(const fs::path& pe, const nlohmann::json& payload) {
    pe::Trailer tr;
    tr.payload = payload.dump();
    return pe::write_trailer(pe, tr);
}

std::optional<nlohmann::json> meta_read_pe_trailer(const fs::path& pe) {
    pe::Trailer tr;
    if (!pe::read_trailer(pe, tr)) return std::nullopt;
    if (tr.payload.empty()) return std::nullopt;
    try {
        return nlohmann::json::parse(tr.payload);
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
}

} // namespace grycegc
