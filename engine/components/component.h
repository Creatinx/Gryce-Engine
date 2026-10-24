#pragma once

#include <nlohmann/json.hpp>

#include <typeindex>
#include <vector>

#include "math/math.h"

namespace gryce_engine::scene {
    class Entity;
} // namespace gryce_engine::scene

namespace gryce_engine::render {
    class RenderContext;
} // namespace gryce_engine::render

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// ComponentTypeView — 组件实例在"某个类型视角"下的指针
//
// 一个组件实例在它自己与各个祖先类型上的子对象地址不一定相同（多继承时必然
// 不同，ScriptComponent 就是全仓唯一的多继承组件）。把派生对象指针直接当
// 基类指针用会取到错误地址。本结构把一次实例化拆成若干"视角"，每个视角
// 记录 (类型, 该类型子对象的正确地址)。
//
// ECS 的组件索引按视角登记：每个实例向它的所有祖先类型各登记一份指针，
// 于是 get_component<Base>() 能命中 get_component<Derived>() 命中的实例，
// 且拿到的是正确的基类子对象地址。
// ---------------------------------------------------------------------------
struct ComponentTypeView {
    std::type_index type;
    void* ptr;
};

// ---------------------------------------------------------------------------
// Component — 所有组件的基类
// 采用 Unity 风格：组件挂载到 Entity 上，可序列化。
// ---------------------------------------------------------------------------
class Component {
public:
    virtual ~Component() = default;

    // 组件类型标识，用于反序列化工厂
    virtual const char* type() const = 0;

    // 序列化/反序列化
    virtual void serialize(nlohmann::json& out) const = 0;
    virtual void deserialize(const nlohmann::json& in) = 0;

    // 生命周期
    virtual void on_attach(scene::Entity* owner) { owner_ = owner; }
    virtual void on_detach() { owner_ = nullptr; }
    virtual void on_awake() {}                      // 组件创建并挂载到 Entity 后
    virtual void on_start() {}                      // 场景开始（所有组件初始化后）
    virtual void on_enable() {}                      // 组件被启用（enabled = true）
    virtual void on_disable() {}                      // 组件被禁用（enabled = false）
    virtual void on_init() {}                         // 场景初始化完成后调用
    virtual void on_update(float dt) { (void)dt; }  // 每帧逻辑更新
    virtual void on_render(render::RenderContext& ctx) { (void)ctx; } // 每帧渲染（可选）
    virtual void on_destroy() {}                      // 组件销毁前

    // 运行时状态快照（用于热重载保留）
    virtual void snapshot_runtime_state(nlohmann::json& out) const { (void)out; }
    virtual void restore_runtime_state(const nlohmann::json& in) { (void)in; }

    // -----------------------------------------------------------------------
    // 类型视角展开（内部机制，供 ECS 索引 / 组件树使用）
    //
    // 契约：本函数必须把本实例在"自身类型 + 所有祖先类型"上的指针全部放进
    // out，每个视角一个条目（类型 + 该类型子对象的正确地址）。
    //
    //   - 自身类型 = 最派生类型（typeid(*this)），指针指向最派生对象本体；
    //   - 祖先类型 = 直接基类，以及基类的祖先，逐级。
    //
    // 虚调用保证 this 已调整到正确的子对象地址，所以各层实现里的
    // static_cast<const void*>(this) 就是该层类型的正确地址，无需手算偏移。
    //
    // 【何时必须覆写】
    // 只要一个类还有派生类，它就必须覆写本函数，并在调用基类实现之后追加
    // 自身这一层：
    //
    //   void collect_type_views(std::vector<ComponentTypeView>& out) const override {
    //       Base::collect_type_views(out);   // 限定调用，非虚，避免递归
    //       out.push_back({ std::type_index(typeid(Derived)),
    //                       const_cast<void*>(static_cast<const void*>(this)) });
    //   }
    //
    // 叶类（没有派生类）不需要覆写：根实现里 typeid(*this) 已经给出动态类型。
    // 漏掉覆写会让"以该类为基类的查询"查不到实例 —— 反射表驱动的回归测试
    // （test/component_tests.cpp 的 hierarchy 套件）会逐条校验，不会漏。
    // -----------------------------------------------------------------------
    virtual void collect_type_views(std::vector<ComponentTypeView>& out) const {
        // 动态（最派生）类型视角。dynamic_cast<const void*> 只读 vtable 中的
        // complete object pointer，不做 type_info 比较，跨 DLL 安全。
        out.push_back({ std::type_index(typeid(*this)),
                        const_cast<void*>(dynamic_cast<const void*>(this)) });
        // Component 视角：this 已是 Component 子对象指针（虚调用保证）。
        out.push_back({ std::type_index(typeid(Component)),
                        const_cast<void*>(static_cast<const void*>(this)) });
    }

    scene::Entity* owner() const { return owner_; }

    bool enabled = true;

    // -----------------------------------------------------------------------
    // 组件树（父子挂载关系）
    //
    // 一个 Entity 上挂载的组件可以形成一棵树（Godot Node 树的组件化版本）：
    //   - 父组件是子组件的组合容器：子组件继承父组件的 enabled 状态
    //     （父禁用 → 整棵子树停摆），子组件的局部变换叠在父组件之上；
    //   - 根组件（没有父组件）的变换以 owner 实体的变换为基准；
    //   - 父组件被移除时，子组件重新挂到祖父（或成为根），不会连带销毁。
    //
    // 这与"基类 / 子类"是两条正交的轴：
    //   基类/子类 = 类型继承（is_derived_from，见 collect_type_views）；
    //   父/子组件 = 实例挂载关系（本节）。
    // 一个 ColorRect 可以既是 BasicRect 的子类，又是某个 Sprite2D 的子组件。
    // -----------------------------------------------------------------------

    // 父组件（nullptr = 组件树的根）
    Component* component_parent() const { return component_parent_; }

    // 子组件（按挂载顺序）
    const std::vector<Component*>& component_children() const { return component_children_; }

    // 挂到父组件下；传 nullptr 表示脱离父组件、成为根。
    // 防环：不允许把组件挂到它自己或它的后代之下（否则树会成环，遍历与序列化
    // 会死循环）。返回 false 表示被防环规则拒绝，树保持原状。
    bool set_component_parent(Component* parent) {
        if (parent == component_parent_) return true;
        for (const Component* cur = parent; cur; cur = cur->component_parent_) {
            if (cur == this) return false;  // 会成环
        }
        // 从旧父上摘下
        if (component_parent_) {
            auto& siblings = component_parent_->component_children_;
            for (auto it = siblings.begin(); it != siblings.end(); ++it) {
                if (*it == this) {
                    siblings.erase(it);
                    break;
                }
            }
        }
        component_parent_ = parent;
        if (parent) parent->component_children_.push_back(this);
        return true;
    }

    // 沿父链走到组件树的根
    Component* component_root() {
        Component* cur = this;
        while (cur->component_parent_) cur = cur->component_parent_;
        return cur;
    }
    const Component* component_root() const {
        const Component* cur = this;
        while (cur->component_parent_) cur = cur->component_parent_;
        return cur;
    }

    // 生命周期继承：自身与整条父链都 enabled 才算有效。
    // 父组件被禁用 → 整棵子树停摆。
    bool is_effectively_enabled() const {
        for (const Component* cur = this; cur; cur = cur->component_parent_) {
            if (!cur->enabled) return false;
        }
        return true;
    }

    // 局部变换（相对父组件；根组件相对 owner 实体）
    const math::Vector3f& local_position() const { return local_position_; }
    const math::Quaternionf& local_rotation() const { return local_rotation_; }
    const math::Vector3f& local_scale() const { return local_scale_; }
    void set_local_position(const math::Vector3f& v) { local_position_ = v; }
    void set_local_rotation(const math::Quaternionf& q) { local_rotation_ = q; }
    void set_local_scale(const math::Vector3f& v) { local_scale_ = v; }

    // 局部变换矩阵
    math::Matrix4f local_matrix() const {
        return math::Matrix4f::translate(local_position_) *
               math::Matrix4f::from_quaternion(local_rotation_) *
               math::Matrix4f::scale(local_scale_);
    }

protected:
    Component() = default;

    scene::Entity* owner_ = nullptr;

private:
    // 组件树的父 / 子指针。owner 相同才构成一棵合法的组件树。
    Component* component_parent_ = nullptr;
    std::vector<Component*> component_children_;

    // 局部变换（默认单位变换）
    math::Vector3f local_position_;
    math::Quaternionf local_rotation_ = math::Quaternionf::identity();
    math::Vector3f local_scale_ = math::Vector3f::one();
};

} // namespace gryce_engine::components
