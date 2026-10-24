#pragma once

#include <nlohmann/json.hpp>

#include <typeindex>
#include <vector>

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

protected:
    Component() = default;

    scene::Entity* owner_ = nullptr;
};

} // namespace gryce_engine::components
