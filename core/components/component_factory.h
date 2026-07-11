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
    std::unique_ptr<Component> create(const std::string& type) const;
    bool has_type(const std::string& type) const;

private:
    ComponentFactory() = default;
    std::unordered_map<std::string, Creator> creators_;
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
