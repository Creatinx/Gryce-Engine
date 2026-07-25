#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include "components/component.h"

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// ComponentFactory — 组件类型注册表
// 通过类型名字符串反序列化时创建对应组件。
// ---------------------------------------------------------------------------
class ComponentFactory {
public:
    using Creator = std::function<std::unique_ptr<Component>()>;

    static ComponentFactory& instance();

    void register_type(const std::string& type, Creator creator);
    void register_type(const std::string& type, Creator creator, const std::string& description);
    std::unique_ptr<Component> create(const std::string& type) const;
    bool has_type(const std::string& type) const;

    // 所有已注册类型名（按注册顺序）
    std::vector<std::string> all_types() const;
    // 组件描述；未注册或没有描述返回空字符串
    const char* description(const std::string& type) const;

private:
    ComponentFactory() = default;

    struct TypeInfo {
        Creator creator;
        std::string description;
    };

    std::unordered_map<std::string, TypeInfo> creators_;
    std::vector<std::string> type_order_;
};

// 注册 helper
struct ComponentRegistrar {
    ComponentRegistrar(const std::string& type, ComponentFactory::Creator creator) {
        ComponentFactory::instance().register_type(type, std::move(creator));
    }
};

// 注册引擎内置组件（Transform、2D 形状、Label 等）
void register_builtin_components();

} // namespace gryce_engine::components
