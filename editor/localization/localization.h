#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace gryce_engine::editor {

// ---------------------------------------------------------------------------
// Localization — 编辑器多语言本地化系统
// ---------------------------------------------------------------------------
// 从项目根目录 `locales/<lang>.json` 加载翻译表。
// 支持运行时切换语言并热重载，不重启编辑器。
// ---------------------------------------------------------------------------

enum class Language {
    English = 0,
    Chinese,
};

const char* language_code(Language lang);
const char* language_display_name(Language lang);

class Localization {
public:
    static Localization& instance();

    // 加载指定语言；失败时保留当前语言/英语兜底。
    bool load(Language lang, const std::string& project_root);

    // 热重载当前语言文件。
    bool reload(const std::string& project_root);

    Language current_language() const { return current_lang_; }

    // 翻译。找不到 key 时返回 key 本身（便于开发时定位）。
    const char* get(const char* key) const;
    const char* get(const std::string& key) const { return get(key.c_str()); }

    // 当前主题是否为浅色（用于日志/UI 颜色自适应）。
    bool is_light_theme() const { return light_theme_; }
    void set_light_theme(bool light) { light_theme_ = light; }

private:
    Localization() = default;

    bool load_file(const std::string& path);

    Language current_lang_ = Language::English;
    std::unordered_map<std::string, std::string> table_;
    bool light_theme_ = false;
};

// 便捷函数
inline const char* tr(const char* key) {
    return Localization::instance().get(key);
}

inline const char* tr(const std::string& key) {
    return Localization::instance().get(key.c_str());
}

} // namespace gryce_engine::editor
