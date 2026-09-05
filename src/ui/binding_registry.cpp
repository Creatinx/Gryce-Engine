#include "GryceEngineUtils/ui/binding_registry.h"

#include <algorithm>
#include <sstream>

#include "utils/glog/glog_lib.h"

namespace GryceEngineUtils::ui {

// ============================================================================
// 单例
// ============================================================================

BindingRegistry& BindingRegistry::instance() {
    static BindingRegistry inst;
    return inst;
}

// ============================================================================
// 数据源操作
// ============================================================================

void BindingRegistry::set_value(const std::string& key, const std::string& value) {
    if (key.empty()) return;

    // 直接存储，不检查旧值（避免 map find 可能的兼容性问题）
    data_[key] = value;
    dirty_keys_.insert(key);
}

void BindingRegistry::set_value(const std::string& key, int32_t value) {
    set_value(key, std::to_string(value));
}

void BindingRegistry::set_value(const std::string& key, float value) {
    set_value(key, std::to_string(value));
}

std::string BindingRegistry::get_value(const std::string& key) const {
    auto it = data_.find(key);
    if (it != data_.end()) {
        return it->second;
    }
    return "";
}

bool BindingRegistry::has_key(const std::string& key) const {
    return data_.find(key) != data_.end();
}

void BindingRegistry::remove_key(const std::string& key) {
    data_.erase(key);
    dirty_keys_.erase(key);
}

// ============================================================================
// 绑定管理
// ============================================================================

void BindingRegistry::bind(Widget* widget, const std::string& prop_name,
                            const std::string& data_key) {
    if (!widget || prop_name.empty() || data_key.empty()) return;

    // 存储绑定记录
    bindings_[data_key].push_back({widget, prop_name, data_key});
    widget_bindings_[widget].push_back({prop_name, data_key});

    // GLOG_DEBUG("[Binding] bound '{}'.{} -> '{}'", data_key, prop_name,
    //            widget->id());

    // 如果数据源已有值，立即初始化控件属性
    auto it = data_.find(data_key);
    if (it != data_.end()) {
        widget->set_property(prop_name.c_str(), it->second.c_str());
    }
}

void BindingRegistry::unbind(Widget* widget) {
    if (!widget) return;

    auto wit = widget_bindings_.find(widget);
    if (wit == widget_bindings_.end()) return;

    // 从 bindings_ 中移除该控件的所有绑定记录
    for (const auto& [prop_name, data_key] : wit->second) {
        auto bit = bindings_.find(data_key);
        if (bit != bindings_.end()) {
            auto& entries = bit->second;
            entries.erase(
                std::remove_if(entries.begin(), entries.end(),
                    [widget, &prop_name](const BindingEntry& e) {
                        return e.widget == widget && e.prop_name == prop_name;
                    }),
                entries.end());
            // 如果没有绑定此 data_key 的控件了，清理空列表
            if (entries.empty()) {
                bindings_.erase(bit);
            }
        }
    }

    widget_bindings_.erase(wit);
}

void BindingRegistry::unbind(Widget* widget, const std::string& prop_name) {
    if (!widget) return;

    auto wit = widget_bindings_.find(widget);
    if (wit == widget_bindings_.end()) return;

    auto& pairs = wit->second;
    auto it = std::find_if(pairs.begin(), pairs.end(),
        [&prop_name](const auto& p) { return p.first == prop_name; });
    if (it == pairs.end()) return;

    const std::string& data_key = it->second;

    // 从 bindings_ 中移除
    auto bit = bindings_.find(data_key);
    if (bit != bindings_.end()) {
        auto& entries = bit->second;
        entries.erase(
            std::remove_if(entries.begin(), entries.end(),
                [widget, &prop_name](const BindingEntry& e) {
                    return e.widget == widget && e.prop_name == prop_name;
                }),
            entries.end());
        if (entries.empty()) {
            bindings_.erase(bit);
        }
    }

    // 从 widget_bindings_ 中移除
    pairs.erase(it);
    if (pairs.empty()) {
        widget_bindings_.erase(wit);
    }
}

size_t BindingRegistry::binding_count(Widget* widget) const {
    auto it = widget_bindings_.find(widget);
    return it != widget_bindings_.end() ? it->second.size() : 0;
}

// ============================================================================
// 脏标记与刷新
// ============================================================================

void BindingRegistry::mark_dirty(const std::string& key) {
    if (data_.find(key) != data_.end()) {
        dirty_keys_.insert(key);
    }
}

void BindingRegistry::mark_all_dirty() {
    for (const auto& [key, _] : data_) {
        dirty_keys_.insert(key);
    }
}

int BindingRegistry::refresh() {
    if (dirty_keys_.empty()) return 0;

    int updated = 0;

    for (const auto& key : dirty_keys_) {
        auto data_it = data_.find(key);
        if (data_it == data_.end()) continue;

        auto bind_it = bindings_.find(key);
        if (bind_it == bindings_.end()) continue;

        const std::string& new_value = data_it->second;

        for (const auto& entry : bind_it->second) {
            if (!entry.widget || !entry.widget->alive()) continue;

            // 调用 set_property 更新控件属性
            if (entry.widget->set_property(entry.prop_name.c_str(), new_value.c_str())) {
                updated++;
                // GLOG_DEBUG("[Binding] refreshed '{}'.{} -> '{}' = \"{}\"",
    //                key, entry.prop_name, entry.widget->id(), new_value);
            }
        }
    }

    dirty_keys_.clear();
    return updated;
}

// ============================================================================
// 全局清理
// ============================================================================

void BindingRegistry::clear_all() {
    data_.clear();
    bindings_.clear();
    widget_bindings_.clear();
    dirty_keys_.clear();
    // GLOG_DEBUG("[Binding] all bindings cleared");
}

size_t BindingRegistry::total_bindings() const {
    size_t count = 0;
    for (const auto& [_, entries] : bindings_) {
        count += entries.size();
    }
    return count;
}

size_t BindingRegistry::dirty_count() const {
    return dirty_keys_.size();
}

} // namespace GryceEngineUtils::ui