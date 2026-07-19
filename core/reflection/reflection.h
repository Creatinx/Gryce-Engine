#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "math/math.h"

// ---------------------------------------------------------------------------
// 组件反射系统（M1-E1）
//
// 编译期宏注册 + 类型擦除字段访问，零外部依赖。
// 设计文档：docs/design/reflection.md
//
// 用法（在 .cpp 中注册）：
//   GRYCE_REFLECT_CLASS(Transform, Component)
//       GRYCE_REFLECT_FIELD(position)
//       GRYCE_REFLECT_FIELD_RANGE(intensity, 0.0f, 100.0f)
//       GRYCE_REFLECT_FIELD_RO(computed_field)
//   GRYCE_REFLECT_END()
//
// 查询：
//   auto fields = reflection::Registry::instance().all_fields("Transform");
//   for (auto* f : fields) { f->name / f->type / read / write ... }
// ---------------------------------------------------------------------------

namespace gryce_engine::reflection {

// ---------------------------------------------------------------------------
// FieldType — 字段类型 tag（Inspector 控件分派依据）
// ---------------------------------------------------------------------------
enum class FieldType {
    Int,
    Float,
    Double,
    Bool,
    String,
    Vector2f,
    Vector3f,
    Vector4f,
    Quaternionf
};

// C++ 类型 → FieldType 映射。未特化的类型触发 static_assert，
// 编译期阻止误注册（如指针/容器/嵌套结构，本轮不支持）。
template<typename T>
constexpr FieldType field_type_of() {
    if constexpr (std::is_same_v<T, int> || std::is_same_v<T, int32_t> ||
                  std::is_same_v<T, uint32_t> || std::is_same_v<T, int64_t>) {
        return FieldType::Int;
    } else if constexpr (std::is_same_v<T, float>) {
        return FieldType::Float;
    } else if constexpr (std::is_same_v<T, double>) {
        return FieldType::Double;
    } else if constexpr (std::is_same_v<T, bool>) {
        return FieldType::Bool;
    } else if constexpr (std::is_same_v<T, std::string>) {
        return FieldType::String;
    } else if constexpr (std::is_same_v<T, math::Vector2f>) {
        return FieldType::Vector2f;
    } else if constexpr (std::is_same_v<T, math::Vector3f>) {
        return FieldType::Vector3f;
    } else if constexpr (std::is_same_v<T, math::Vector4f>) {
        return FieldType::Vector4f;
    } else if constexpr (std::is_same_v<T, math::Quaternionf>) {
        return FieldType::Quaternionf;
    } else {
        static_assert(!sizeof(T*), "field_type_of: 未映射的字段类型（嵌套结构/容器/enum 本轮不支持）");
    }
}

// ---------------------------------------------------------------------------
// FieldInfo — 字段元信息 + 类型擦除读写
// 不用 offset：组件含虚函数、非 standard-layout，offsetof 不合法；
// 注册时由成员指针生成 lambda，对继承字段同样安全。
// ---------------------------------------------------------------------------
struct FieldInfo {
    std::string name;
    std::string display_name;   // Inspector 显示名（默认 = name）
    FieldType type = FieldType::Float;
    bool read_only = false;
    bool has_range = false;
    float range_min = 0.0f;
    float range_max = 0.0f;

    // obj 为组件对象地址；dst/src 为对应 C++ 类型对象地址
    std::function<void(const void* obj, void* dst)> read;
    std::function<bool(void* obj, const void* src)> write;  // read_only 时恒 false
};

// ---------------------------------------------------------------------------
// TypeInfo — 类型元信息（仅本类声明的字段；继承链由 Registry 展开）
// ---------------------------------------------------------------------------
struct TypeInfo {
    std::string name;
    std::string parent_name;
    std::vector<FieldInfo> fields;
};

// ---------------------------------------------------------------------------
// Registry — 类型注册表（函数内 static，跨 TU 初始化顺序安全）
// ---------------------------------------------------------------------------
class Registry {
public:
    static Registry& instance() {
        static Registry registry;
        return registry;
    }

    TypeInfo& register_type(const std::string& name, const std::string& parent_name) {
        auto& info = types_[name];
        info.name = name;
        info.parent_name = parent_name;
        return info;
    }

    const TypeInfo* find(const std::string& name) const {
        auto it = types_.find(name);
        return it != types_.end() ? &it->second : nullptr;
    }

    // 继承链展开：基类字段在前（Component::enabled），派生类字段在后。
    // 未注册类型返回空 vector。
    std::vector<const FieldInfo*> all_fields(const std::string& name) const {
        std::vector<const FieldInfo*> out;
        // 先沿 parent 链走到根，再反向收集（根 → 叶）
        std::vector<const TypeInfo*> chain;
        const TypeInfo* cur = find(name);
        while (cur) {
            chain.push_back(cur);
            cur = cur->parent_name.empty() ? nullptr : find(cur->parent_name);
        }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            for (const auto& f : (*it)->fields) {
                out.push_back(&f);
            }
        }
        return out;
    }

    // 已注册类型数量（测试/调试）
    size_t type_count() const { return types_.size(); }

private:
    Registry() = default;
    std::unordered_map<std::string, TypeInfo> types_;
};

// ---------------------------------------------------------------------------
// TypeBuilder — 注册期链式构造器（宏展开的目标）
// ---------------------------------------------------------------------------
template<typename C>
class TypeBuilder {
public:
    TypeBuilder(const char* name, const char* parent) {
        info_ = &Registry::instance().register_type(name, parent);
    }

    // 可读写字段
    template<typename M>
    TypeBuilder& add_field(const char* name, M C::* mp) {
        FieldInfo f = make_field<M>(name, mp, false);
        info_->fields.push_back(std::move(f));
        return *this;
    }

    // 只读字段（Inspector 禁用态；write 恒返回 false）
    template<typename M>
    TypeBuilder& add_field_readonly(const char* name, M C::* mp) {
        FieldInfo f = make_field<M>(name, mp, true);
        info_->fields.push_back(std::move(f));
        return *this;
    }

    // 带 Inspector 滑条范围的标量字段
    template<typename M>
    TypeBuilder& add_field_ranged(const char* name, M C::* mp, float min_val, float max_val) {
        FieldInfo f = make_field<M>(name, mp, false);
        f.has_range = true;
        f.range_min = min_val;
        f.range_max = max_val;
        info_->fields.push_back(std::move(f));
        return *this;
    }

    bool commit() const { return info_ != nullptr; }

private:
    template<typename M>
    static FieldInfo make_field(const char* name, M C::* mp, bool read_only) {
        FieldInfo f;
        f.name = name;
        f.display_name = name;
        f.type = field_type_of<M>();
        f.read_only = read_only;
        f.read = [mp](const void* obj, void* dst) {
            *static_cast<M*>(dst) = static_cast<const C*>(obj)->*mp;
        };
        if (read_only) {
            f.write = [](void*, const void*) { return false; };
        } else {
            f.write = [mp](void* obj, const void* src) {
                static_cast<C*>(obj)->*mp = *static_cast<const M*>(src);
                return true;
            };
        }
        return f;
    }

    TypeInfo* info_ = nullptr;
};

// ---------------------------------------------------------------------------
// 类型擦除读写助手（调用方应先比对 f.type == field_type_of<T>()）
// ---------------------------------------------------------------------------
template<typename T>
T read_field(const void* obj, const FieldInfo& f) {
    T value{};
    if (obj && f.read) f.read(obj, &value);
    return value;
}

template<typename T>
bool write_field(void* obj, const FieldInfo& f, const T& value) {
    if (!obj || !f.write) return false;
    return f.write(obj, &value);
}

// 内建组件反射注册锚点（core/reflection/builtin_reflections.cpp）。
// gryce_core 为静态库：注册静态对象所在 TU 若无符号被引用会被链接器丢弃，
// register_builtin_components() 会调用本函数强制链接该 TU。
void register_builtin_reflections();

} // namespace gryce_engine::reflection

// ---------------------------------------------------------------------------
// 注册宏（一行式；在 .cpp 文件作用域使用）
// ---------------------------------------------------------------------------
#define GRYCE_REFLECT_CLASS(Class, Base) \
    namespace gryce_reflect_scope_##Class { \
    using GRYCE_ReflectCurrent = Class; \
    static const bool registered = \
        ::gryce_engine::reflection::TypeBuilder<Class>(#Class, #Base)

#define GRYCE_REFLECT_FIELD(field) \
        .add_field(#field, &GRYCE_ReflectCurrent::field)

#define GRYCE_REFLECT_FIELD_RO(field) \
        .add_field_readonly(#field, &GRYCE_ReflectCurrent::field)

#define GRYCE_REFLECT_FIELD_RANGE(field, min_val, max_val) \
        .add_field_ranged(#field, &GRYCE_ReflectCurrent::field, min_val, max_val)

#define GRYCE_REFLECT_END() \
        .commit(); \
    }
