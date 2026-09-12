#pragma once

#include <string>
#include <unordered_map>

namespace gryce_engine::editor {

// ---------------------------------------------------------------------------
// I18n — 界面文本本地化单例。
// 键即英文原文，`tr(key)` 在 zh 语言下返回对应中文文本，其余语言返回 key 本身。
// 翻译表为单向 `{英文key: 中文文本}` 映射，无需维护英文表，零侵入替换。
// ---------------------------------------------------------------------------
class I18n {
public:
    static I18n& instance();

    // 语言代码："zh" 加载中文翻译表，其余语言使用原文。
    void set_language(const std::string& lang);
    const std::string& language() const { return language_; }

    // 返回翻译文本（const char* 便于直接传给 ImGui）。
    const char* tr(const char* key) const;
    // std::string 版本，用于 %s 格式化拼接等场景。
    std::string tr_str(const char* key) const { return std::string(tr(key)); }

private:
    I18n() = default;

    std::string language_ = "zh";
    std::unordered_map<std::string, std::string> zh_table_;
    void load_zh_table();
};

} // namespace gryce_engine::editor