#pragma once

// GryceEngineUtils/ui/binding_registry.h
//
// BindingRegistry — 数据绑定单例
//
// 功能：
// 1. 注册数据源（key-value 形式）
// 2. 将 UI 控件属性绑定到数据源
// 3. 脏标记刷新机制：数据变化时自动更新绑定的控件属性
// 4. 控件销毁时自动解绑
//
// 用法：
//   auto& br = BindingRegistry::instance();
//   br.set_value("playerName", "Alice");
//   br.bind(myButton, "text", "playerName");
//   br.set_value("playerName", "Bob");  // 自动标记脏
//   br.refresh();  // 更新 "playerName" 绑定的所有控件
//
// .uif 中绑定：
//   <Button bind:text="playerName" />
//   <Text bind:text="scoreDisplay" />
//   <Panel bind:visible="isPanelVisible" />

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "GryceEngineUtils/ui/widget.h"

namespace GryceEngineUtils::ui {

// ---------------------------------------------------------------------------
// BindingRegistry — 数据绑定单例
// ---------------------------------------------------------------------------
class BindingRegistry {
public:
    // 单例访问
    static BindingRegistry& instance();

    // ======================================================================
    // 数据源操作
    // ======================================================================

    // 设置数据源值（标记为脏）
    void set_value(const std::string& key, const std::string& value);
    void set_value(const std::string& key, int32_t value);
    void set_value(const std::string& key, float value);
    // 注意：bool 重载会与 const char* 参数产生歧义（const char* 优先转换为 bool 而非 std::string）
    // 请使用 set_value(key, "true") 或 set_value(key, "false") 代替
    // void set_value(const std::string& key, bool value);

    // 获取数据源值（字符串形式）
    std::string get_value(const std::string& key) const;

    // 检查数据源是否存在
    bool has_key(const std::string& key) const;

    // 删除数据源
    void remove_key(const std::string& key);

    // ======================================================================
    // 绑定管理
    // ======================================================================

    // 注册绑定：当 data_key 变化时，更新 widget 的 prop_name 属性
    // 如果 data_key 已有值，立即初始化 widget 属性
    void bind(Widget* widget, const std::string& prop_name,
              const std::string& data_key);

    // 解绑控件的所有绑定（控件销毁时自动调用）
    void unbind(Widget* widget);

    // 解绑控件的指定属性绑定
    void unbind(Widget* widget, const std::string& prop_name);

    // 返回控件当前的绑定数量
    size_t binding_count(Widget* widget) const;

    // ======================================================================
    // 脏标记与刷新
    // ======================================================================

    // 标记数据源为脏（强制下次 refresh 更新）
    void mark_dirty(const std::string& key);

    // 标记所有数据源为脏
    void mark_all_dirty();

    // 刷新所有脏标记的绑定
    // 返回被更新的控件数量
    int refresh();

    // ======================================================================
    // 全局清理
    // ======================================================================

    // 清除所有绑定和数据
    void clear_all();

    // 总绑定数（调试用）
    size_t total_bindings() const;

    // 总脏键数（调试用）
    size_t dirty_count() const;

private:
    BindingRegistry() = default;
    ~BindingRegistry() = default;
    BindingRegistry(const BindingRegistry&) = delete;
    BindingRegistry& operator=(const BindingRegistry&) = delete;

    // 单条绑定记录
    struct BindingEntry {
        Widget* widget = nullptr;
        std::string prop_name;   // 控件属性名（如 "text", "visible"）
        std::string data_key;    // 数据源键名
    };

    // 数据存储（统一以字符串形式存储）
    std::unordered_map<std::string, std::string> data_;

    // data_key -> 绑定到此数据源的控件列表
    std::unordered_map<std::string, std::vector<BindingEntry>> bindings_;

    // widget -> 该控件绑定的所有 (prop_name, data_key) 对
    // 用于快速解绑
    std::unordered_map<Widget*, std::vector<std::pair<std::string, std::string>>>
        widget_bindings_;

    // 脏键集合
    std::unordered_set<std::string> dirty_keys_;
};

} // namespace GryceEngineUtils::ui