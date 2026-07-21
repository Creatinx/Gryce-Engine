#include "localization.h"

#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "utils/glog/glog_lib.h"

namespace gryce_engine::editor {

const char* language_code(Language lang) {
    switch (lang) {
        case Language::Chinese:  return "zh";
        default:                 return "en";
    }
}

const char* language_display_name(Language lang) {
    switch (lang) {
        case Language::Chinese:  return "\u4e2d\u6587";   // 中文
        default:                 return "English";
    }
}

Localization& Localization::instance() {
    static Localization loc;
    return loc;
}

bool Localization::load_file(const std::string& path) {
    if (!std::filesystem::exists(path)) {
        GLOG_WARN("Localization: locale file not found '{}'", path);
        return false;
    }
    std::ifstream ifs(path);
    if (!ifs) {
        GLOG_ERROR("Localization: failed to open '{}'", path);
        return false;
    }
    try {
        nlohmann::json j = nlohmann::json::parse(ifs);
        if (!j.is_object()) {
            GLOG_ERROR("Localization: root of '{}' must be an object", path);
            return false;
        }
        table_.clear();
        for (auto& [key, value] : j.items()) {
            if (value.is_string()) {
                table_[key] = value.get<std::string>();
            }
        }
    } catch (const std::exception& e) {
        GLOG_ERROR("Localization: failed to parse '{}': {}", path, e.what());
        return false;
    }
    return true;
}

bool Localization::load(Language lang, const std::string& project_root) {
    std::string path = project_root + "/locales/" + language_code(lang) + ".json";
    if (!load_file(path)) {
        if (lang != Language::English) {
            // 非英语失败时回退到英语
            GLOG_WARN("Localization: falling back to English");
            return load(Language::English, project_root);
        }
        return false;
    }
    current_lang_ = lang;
    GLOG_INFO("Localization: loaded language '{}' from '{}'", language_code(lang), path);
    return true;
}

bool Localization::reload(const std::string& project_root) {
    return load(current_lang_, project_root);
}

const char* Localization::get(const char* key) const {
    auto it = table_.find(key);
    if (it != table_.end()) {
        return it->second.c_str();
    }
    return key;
}

} // namespace gryce_engine::editor
