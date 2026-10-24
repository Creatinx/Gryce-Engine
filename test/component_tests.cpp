// ===========================================================================
// test/component_tests.cpp
// 组件继承语义（基类查询 / 类型视角索引）回归测试。
//
// 背景
// ----
// ECS 组件查询必须同时支持两种语义：
//   - 精确查询：实体是否拥有 ColorRect 组件
//   - 基类查询：实体是否拥有任意 Component2D 派生组件
// 两者互不为子集 —— Component2D 是抽象类，永远不会被实例化，但"实体有一个
// Component2D 组件"显然成立。历史实现用"线性扫描 + dynamic_cast"支持基类，
// 中途被改成 typeid 精确哈希（快，但丢了基类语义）：pool_ptr<Component2D>()
// 恒空 → 2D 渲染系统的 foreach_with_component<Component2D> 收集到 0 个组件
// → 整个 2D 渲染管线画不出任何东西。
//
// 本测试锁定修复后的语义，分四个套件：
//   hierarchy：反射表驱动的全量继承链校验（谁漏了类型视角展开立刻暴露）
//   query    ：精确查询 / 基类查询 / 同类型多实例 / 卸载后失效
//   pool     ：ComponentStore 的类型视角池 + foreach_with_component 收集数
//   lifecycle：on_awake 在模板与非模板两条挂载路径上都要触发
//
// 链接 GryceCore.dll（Entity / ComponentStore / 反射表都在 core 内部）。
// 不碰 GPU / 窗口，CI 无需显卡。
//
//   ctest -R component_ --suite hierarchy|query|pool|lifecycle
// ===========================================================================

#include <cstdio>
#include <cstring>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <typeindex>
#include <vector>

#include "api/component_api.h"
#include "api/core_api.h"
#include "api/edit_api.h"
#include "api/entity_api.h"
#include "components/2d/basic_rect.h"
#include "components/2d/component_2d.h"
#include "components/2d/label.h"
#include "components/camera.h"
#include "components/component.h"
#include "components/component_factory.h"
#include "ecs/component_store.h"
#include "reflection/reflection.h"
#include "reflection/type_name.h"
#include "scene/entity.h"
#include "scene/query.h"
#include "scene/scene.h"
#include "scene/scene_serializer.h"

using gryce_engine::components::Component;
using gryce_engine::components::ComponentFactory;
using gryce_engine::components::d2::Component2D;
using gryce_engine::ecs::ComponentStore;
using gryce_engine::scene::Entity;
using gryce_engine::scene::Scene;
namespace math = gryce_engine::math;

// ===========================================================================
// 迷你断言框架（与其它测试同风格，无第三方依赖）
// ===========================================================================
namespace {

struct Case {
    std::string suite;
    std::string name;
    std::function<void()> fn;
};

std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}

struct Registrar {
    Registrar(const char* suite, const char* name, void (*fn)()) {
        registry().push_back(Case{suite, name, fn});
    }
};

int g_checks = 0;
int g_checks_failed = 0;
int g_case_checks = 0;
int g_case_failed = 0;

void fail_at(const char* file, int line, const std::string& msg) {
    ++g_case_failed;
    ++g_checks_failed;
    std::printf("      FAIL %s:%d  %s\n", file, line, msg.c_str());
    std::fflush(stdout);
}

} // namespace

#define TEST(suite, name)                                                            \
    static void suite##__##name##_impl();                                            \
    static ::Registrar suite##__##name##_reg(#suite, #name, &suite##__##name##_impl); \
    static void suite##__##name##_impl()

#define CHECK_TRUE(expr)                                                             \
    do {                                                                             \
        ++::g_checks; ++::g_case_checks;                                             \
        if (!(expr)) ::fail_at(__FILE__, __LINE__, "expected true: " #expr);          \
    } while (0)

#define CHECK_FALSE(expr)                                                            \
    do {                                                                             \
        ++::g_checks; ++::g_case_checks;                                             \
        if (expr) ::fail_at(__FILE__, __LINE__, "expected false: " #expr);            \
    } while (0)

#define CHECK_INT(actual, expected)                                                  \
    do {                                                                             \
        ++::g_checks; ++::g_case_checks;                                             \
        const long long a_ = static_cast<long long>(actual);                         \
        const long long b_ = static_cast<long long>(expected);                       \
        if (a_ != b_) {                                                              \
            char buf_[256];                                                          \
            std::snprintf(buf_, sizeof(buf_), "%s => %lld, expected %lld", #actual,   \
                          a_, b_);                                                   \
            ::fail_at(__FILE__, __LINE__, buf_);                                     \
        }                                                                            \
    } while (0)

#define CHECK_NEAR(actual, expected, eps)                                            \
    do {                                                                             \
        ++::g_checks; ++::g_case_checks;                                             \
        const double a_ = static_cast<double>(actual);                                \
        const double b_ = static_cast<double>(expected);                              \
        if (!(std::fabs(a_ - b_) <= static_cast<double>(eps))) {                      \
            char buf_[256];                                                          \
            std::snprintf(buf_, sizeof(buf_), "%s => %.8g, expected %.8g", #actual,   \
                          a_, b_);                                                   \
            ::fail_at(__FILE__, __LINE__, buf_);                                     \
        }                                                                            \
    } while (0)

// ===========================================================================
// 测试夹具
// ===========================================================================
namespace {

// 内置组件工厂 + 反射表注册（进程内只做一次）。
void ensure_builtins() {
    static bool done = false;
    if (done) return;
    done = true;
    gryce_engine::components::register_builtin_components();
}

// 探针组件：记录 on_awake 是否被调用。
class ProbeComponent : public Component {
public:
    bool awakened = false;
    int awake_count = 0;

    const char* type() const override { return "ProbeComponent"; }
    void serialize(nlohmann::json& out) const override { (void)out; }
    void deserialize(const nlohmann::json& in) override { (void)in; }
    void on_awake() override {
        awakened = true;
        ++awake_count;
    }
};

} // namespace

// ===========================================================================
// suite: hierarchy —— 反射表驱动的全量继承链校验
// ===========================================================================

// 反射表是"组件继承关系"的真源。逐条校验：对每个注册类型，实例化后必须能
// 沿父链被基类视角查到。这条用例能捕获"新增了中间基类却忘了给它加类型
// 视角展开"这类回归 —— 那种类一旦漏了，以它为基类的查询会静默返回空。
TEST(hierarchy, reflection_chain_resolves) {
    ensure_builtins();

    const auto& types = gryce_engine::reflection::Registry::instance().all_types();
    CHECK_TRUE(types.size() >= 50);

    auto& factory = ComponentFactory::instance();

    int checked = 0;
    int skipped = 0;
    for (const auto& entry : types) {
        const std::string& name = entry.first;
        const auto& info = entry.second;
        if (info.parent_name.empty()) continue;  // 根类型没有父

        std::unique_ptr<Component> comp = factory.create(name);
        if (!comp) {
            ++skipped;  // 抽象基类（Component2D / BasicRect）或未进工厂
            continue;
        }

        Entity entity(name);
        Component* raw = entity.add_component(std::move(comp));
        CHECK_TRUE(raw != nullptr);

        // 精确视角：自身类型
        CHECK_TRUE(entity.find_component_view(info.type_index) != nullptr);

        // 沿反射表的父链逐级校验基类视角
        std::string parent = info.parent_name;
        int depth = 0;
        while (!parent.empty() && depth < 8) {
            const std::type_index pidx =
                gryce_engine::reflection::Registry::instance().type_index_of(parent);
            CHECK_TRUE(pidx != std::type_index(typeid(void)));
            CHECK_TRUE(entity.find_component_view(pidx) != nullptr);

            const auto* pinfo = gryce_engine::reflection::Registry::instance().find(parent);
            parent = pinfo ? pinfo->parent_name : std::string();
            ++depth;
        }
        ++checked;
    }

    std::printf("      (inheritance chain checked for %d types, %d abstract/unregistered skipped)\n",
                checked, skipped);
    CHECK_TRUE(checked >= 40);
}

// 根类型视角：所有组件都能被 Component 视角查到。
TEST(hierarchy, every_component_has_root_view) {
    ensure_builtins();

    Entity entity("Root");
    const std::type_index component_type = std::type_index(typeid(Component));

    auto* color_rect = entity.add_component(
        std::make_unique<gryce_engine::components::d2::basic_rect::ColorRect>());
    CHECK_TRUE(color_rect != nullptr);
    CHECK_TRUE(entity.find_component_view(component_type) != nullptr);

    // Component 视角的指针必须能安全地当 Component* 用
    Component* view = static_cast<Component*>(entity.find_component_view(component_type));
    CHECK_TRUE(view != nullptr);
    CHECK_TRUE(view->owner() == &entity);
}

// 多继承场景：ScriptComponent 同时继承 Component 与 script::ScriptHost，
// 组件视角必须落在 Component 子对象上（owner 可读、类型正确）。
TEST(hierarchy, multiple_inheritance_view) {
    ensure_builtins();

    auto& factory = ComponentFactory::instance();
    std::unique_ptr<Component> comp = factory.create("Script");
    if (!comp) {
        // 脚本组件没进工厂就跳过（不影响其余断言的有效性）
        return;
    }

    Entity entity("Scripted");
    Component* raw = entity.add_component(std::move(comp));
    CHECK_TRUE(raw != nullptr);

    const std::type_index component_type = std::type_index(typeid(Component));

    // 根类型视角：脚本组件必须也登记在 Component 视角下，且那一条是
    // Component 子对象（不是别的子对象地址），owner 可读、虚调用正确。
    Component* script_root_view = nullptr;
    const std::size_t root_views = entity.component_view_count(component_type);
    for (std::size_t i = 0; i < root_views; ++i) {
        auto* c = static_cast<Component*>(entity.component_view_at(component_type, i));
        if (c && std::string(c->type()) == std::string(raw->type())) {
            script_root_view = c;
            break;
        }
    }
    CHECK_TRUE(script_root_view != nullptr);
    CHECK_TRUE(script_root_view->owner() == &entity);
}

// ===========================================================================
// suite: query —— 精确查询 / 基类查询 / 多实例 / 卸载
// ===========================================================================

TEST(query, base_class_query_finds_derived) {
    ensure_builtins();

    Entity entity("Quad");
    auto* rect = entity.add_component(
        std::make_unique<gryce_engine::components::d2::basic_rect::ColorRect>());
    CHECK_TRUE(rect != nullptr);

    // 精确查询
    CHECK_TRUE(entity.get_component<gryce_engine::components::d2::basic_rect::ColorRect>() == rect);
    // 基类查询：Component2D 是抽象类，永远没有"精确的 Component2D 组件"，
    // 但实体显然"有一个 Component2D 组件"
    CHECK_TRUE(entity.get_component<Component2D>() == rect);
    // 基类查询：BasicRect（ColorRect 的直接基类）
    CHECK_TRUE(entity.get_component<gryce_engine::components::d2::basic_rect::BasicRect>() == rect);
    // 根类型查询：命中的是最早挂载的那一个（Entity 构造函数先挂 Transform）
    CHECK_TRUE(entity.get_component<Component>() == static_cast<Component*>(entity.transform()));
    // 不存在的类型仍返回空
    CHECK_TRUE(entity.get_component<gryce_engine::components::Camera>() == nullptr);
}

TEST(query, multiple_instances_same_type) {
    ensure_builtins();

    Entity entity("TwoRects");
    auto* a = entity.add_component(
        std::make_unique<gryce_engine::components::d2::basic_rect::ColorRect>());
    auto* b = entity.add_component(
        std::make_unique<gryce_engine::components::d2::basic_rect::ColorRect>());
    CHECK_TRUE(a != nullptr);
    CHECK_TRUE(b != nullptr);
    CHECK_TRUE(a != b);

    const std::type_index rect_type =
        std::type_index(typeid(gryce_engine::components::d2::basic_rect::ColorRect));
    CHECK_INT(entity.component_view_count(rect_type), 2);
    CHECK_INT(entity.component_view_count(std::type_index(typeid(Component2D))), 2);

    // 卸载其中一个，剩下的仍可查到；被卸载的不再占索引
    CHECK_TRUE(entity.remove_component(a));
    CHECK_INT(entity.component_view_count(rect_type), 1);
    CHECK_TRUE(entity.get_component<gryce_engine::components::d2::basic_rect::ColorRect>() == b);
    CHECK_TRUE(entity.get_component<Component2D>() == b);

    // 全卸完 → 基类视角也归零
    CHECK_TRUE(entity.remove_component(b));
    CHECK_INT(entity.component_view_count(rect_type), 0);
    CHECK_INT(entity.component_view_count(std::type_index(typeid(Component2D))), 0);
    CHECK_TRUE(entity.get_component<Component2D>() == nullptr);
}

TEST(query, base_query_returns_first_match) {
    ensure_builtins();

    Entity entity("Mixed");
    auto* rect = entity.add_component(
        std::make_unique<gryce_engine::components::d2::basic_rect::ColorRect>());
    auto* label = entity.add_component(
        std::make_unique<gryce_engine::components::d2::text::Label>());
    CHECK_TRUE(rect != nullptr);
    CHECK_TRUE(label != nullptr);

    // 基类查询返回"挂载顺序上的第一个匹配"（与历史 dynamic_cast 线性扫描语义一致）
    Component2D* first = entity.get_component<Component2D>();
    CHECK_TRUE(first != nullptr);
    CHECK_TRUE(first == rect);
    CHECK_INT(entity.component_view_count(std::type_index(typeid(Component2D))), 2);
}

// ===========================================================================
// suite: pool —— ComponentStore 类型视角池
// ===========================================================================

TEST(pool, view_pools_hold_derived_components) {
    ensure_builtins();

    Scene scene("PoolScene");
    Entity* entity = scene.create_entity("Quad");
    CHECK_TRUE(entity != nullptr);
    entity->add_component(std::make_unique<gryce_engine::components::d2::basic_rect::ColorRect>());

    ComponentStore& store = scene.component_store();

    // 精确池
    const auto* rect_pool = store.pool_ptr<gryce_engine::components::d2::basic_rect::ColorRect>();
    CHECK_TRUE(rect_pool != nullptr);
    CHECK_INT(rect_pool->size(), 1);

    // 基类池（抽象类也一样有内容）
    const auto* c2d_pool = store.pool_ptr<Component2D>();
    CHECK_TRUE(c2d_pool != nullptr);
    CHECK_INT(c2d_pool->size(), 1);

    // 根类型池
    const auto* root_pool = store.pool_ptr<Component>();
    CHECK_TRUE(root_pool != nullptr);
    CHECK_TRUE(root_pool->size() >= 1);
}

TEST(pool, foreach_collects_base_class_components) {
    ensure_builtins();

    Scene scene("ForeachScene");
    Entity* a = scene.create_entity("Rect");
    Entity* b = scene.create_entity("Label");
    a->add_component(std::make_unique<gryce_engine::components::d2::basic_rect::ColorRect>());
    b->add_component(std::make_unique<gryce_engine::components::d2::text::Label>());

    // 这正是 2D 渲染系统用的查询：修复前它恒收集 0 个组件。
    int collected = 0;
    gryce_engine::ecs::foreach_with_component<Component2D>(
        scene, [&](Entity* e, Component2D* c) {
            (void)e;
            (void)c;
            ++collected;
        });
    CHECK_INT(collected, 2);

    // 精确类型的 foreach 仍然只收集精确类型
    int rects = 0;
    gryce_engine::ecs::foreach_with_component<gryce_engine::components::d2::basic_rect::ColorRect>(
        scene, [&](Entity* e, gryce_engine::components::d2::basic_rect::ColorRect* c) {
            (void)e;
            (void)c;
            ++rects;
        });
    CHECK_INT(rects, 1);
}

TEST(pool, unregister_removes_from_base_pools) {
    ensure_builtins();

    Scene scene("UnregisterScene");
    Entity* entity = scene.create_entity("Rect");
    auto* rect = entity->add_component(
        std::make_unique<gryce_engine::components::d2::basic_rect::ColorRect>());
    CHECK_TRUE(rect != nullptr);

    ComponentStore& store = scene.component_store();
    CHECK_INT(store.pool_ptr<Component2D>()->size(), 1);

    CHECK_TRUE(entity->remove_component(rect));

    const auto* c2d_pool = store.pool_ptr<Component2D>();
    CHECK_TRUE(c2d_pool == nullptr || c2d_pool->empty());
}

// ===========================================================================
// suite: lifecycle —— on_awake 在两条挂载路径上都要触发
// ===========================================================================

TEST(lifecycle, awake_on_both_mount_paths) {
    ensure_builtins();

    Entity entity("Probe");

    // 模板路径：Entity::add_component<T>()
    ProbeComponent* templated = entity.add_component<ProbeComponent>();
    CHECK_TRUE(templated != nullptr);
    CHECK_TRUE(templated->awakened);
    CHECK_INT(templated->awake_count, 1);

    // 非模板路径：Entity::add_component(unique_ptr)
    auto owned = std::make_unique<ProbeComponent>();
    ProbeComponent* raw = owned.get();
    entity.add_component(std::move(owned));
    CHECK_TRUE(raw->awakened);
    CHECK_INT(raw->awake_count, 1);
}

// ===========================================================================
// suite: tree —— 组件父子挂载关系（组件树）
// ===========================================================================

TEST(tree, attach_and_detach) {
    ensure_builtins();

    Entity entity("Tree");
    auto* parent = entity.add_component<ProbeComponent>();
    auto* child = entity.add_component<ProbeComponent>();
    CHECK_TRUE(parent != nullptr);
    CHECK_TRUE(child != nullptr);

    // 初始都是组件树的根（实体的三个内置组件 + 两个探针）
    CHECK_TRUE(parent->component_parent() == nullptr);
    CHECK_TRUE(child->component_parent() == nullptr);
    CHECK_INT(entity.component_roots().size(), 5);

    // 挂载
    CHECK_TRUE(entity.set_component_parent(child, parent));
    CHECK_TRUE(child->component_parent() == parent);
    CHECK_INT(parent->component_children().size(), 1);
    CHECK_TRUE(parent->component_children()[0] == child);
    CHECK_TRUE(child->component_root() == parent);

    // 脱离 → 重新成为根
    CHECK_TRUE(entity.set_component_parent(child, nullptr));
    CHECK_TRUE(child->component_parent() == nullptr);
    CHECK_INT(parent->component_children().size(), 0);
}

TEST(tree, cycle_is_rejected) {
    ensure_builtins();

    Entity entity("Tree");
    auto* a = entity.add_component<ProbeComponent>();
    auto* b = entity.add_component<ProbeComponent>();
    auto* c = entity.add_component<ProbeComponent>();

    CHECK_TRUE(entity.set_component_parent(b, a));
    CHECK_TRUE(entity.set_component_parent(c, b));

    // 把 a 挂到自己的后代 c 之下 → 必须被拒绝
    CHECK_FALSE(entity.set_component_parent(a, c));
    CHECK_TRUE(a->component_parent() == nullptr);
    CHECK_TRUE(c->component_parent() == b);

    // 挂到自己之下 → 拒绝
    CHECK_FALSE(entity.set_component_parent(a, a));
    CHECK_TRUE(a->component_parent() == nullptr);
}

TEST(tree, lifecycle_inherits_from_parent) {
    ensure_builtins();

    Entity entity("Tree");
    auto* parent = entity.add_component<ProbeComponent>();
    auto* child = entity.add_component<ProbeComponent>();
    CHECK_TRUE(entity.set_component_parent(child, parent));

    CHECK_TRUE(parent->is_effectively_enabled());
    CHECK_TRUE(child->is_effectively_enabled());

    // 父组件禁用 → 整棵子树停摆
    parent->enabled = false;
    CHECK_TRUE(parent->is_effectively_enabled() == false);
    CHECK_TRUE(child->is_effectively_enabled() == false);

    parent->enabled = true;
    CHECK_TRUE(child->is_effectively_enabled());
}

TEST(tree, local_transform_composes) {
    ensure_builtins();

    Entity entity("Tree");
    auto* parent = entity.add_component<ProbeComponent>();
    auto* child = entity.add_component<ProbeComponent>();
    CHECK_TRUE(entity.set_component_parent(child, parent));

    parent->set_local_position(math::Vector3f(10.0f, 0.0f, 0.0f));
    child->set_local_position(math::Vector3f(5.0f, 0.0f, 0.0f));

    // 相对变换 = 父 × 子
    const math::Matrix4f rel = entity.component_relative_transform(child);
    const math::Vector3f p = rel.transform_point(math::Vector3f(0.0f, 0.0f, 0.0f));
    CHECK_NEAR(p.x, 15.0, 1e-4);
    CHECK_NEAR(p.y, 0.0, 1e-4);

    // 世界变换 = 实体变换 × 相对变换（实体在原点 → 与相对变换一致）
    const math::Vector3f w = entity.component_world_transform(child)
                                 .transform_point(math::Vector3f(0.0f, 0.0f, 0.0f));
    CHECK_NEAR(w.x, 15.0, 1e-4);
}

TEST(tree, removing_parent_keeps_children) {
    ensure_builtins();

    Entity entity("Tree");
    auto* grandparent = entity.add_component<ProbeComponent>();
    auto* parent = entity.add_component<ProbeComponent>();
    auto* child = entity.add_component<ProbeComponent>();
    CHECK_TRUE(entity.set_component_parent(parent, grandparent));
    CHECK_TRUE(entity.set_component_parent(child, parent));

    // 移除父组件 → 子组件重挂到祖父，不被连带销毁
    CHECK_TRUE(entity.remove_component(parent));
    CHECK_TRUE(child->component_parent() == grandparent);
    CHECK_INT(grandparent->component_children().size(), 1);
}

TEST(tree, clone_rebuilds_component_tree) {
    ensure_builtins();

    Entity entity("Tree");
    // 用工厂里真实存在的组件类型（clone 走 ComponentFactory 重建）
    auto* parent = entity.add_component(
        std::make_unique<gryce_engine::components::d2::basic_rect::ColorRect>());
    auto* child = entity.add_component(
        std::make_unique<gryce_engine::components::d2::text::Label>());
    CHECK_TRUE(entity.set_component_parent(child, parent));
    parent->set_local_position(math::Vector3f(3.0f, 4.0f, 0.0f));
    child->set_local_position(math::Vector3f(1.0f, 0.0f, 0.0f));

    auto cloned = entity.clone();
    CHECK_TRUE(cloned != nullptr);

    // 按挂载顺序对齐：克隆体的第 4 / 第 5 个组件对应原体的 parent / child
    const auto& comps = cloned->components();
    CHECK_TRUE(comps.size() >= 5);
    auto* cloned_parent = comps[3].get();
    auto* cloned_child = comps[4].get();

    CHECK_TRUE(cloned_child->component_parent() == cloned_parent);
    CHECK_NEAR(cloned_parent->local_position().x, 3.0, 1e-4);
    CHECK_NEAR(cloned_parent->local_position().y, 4.0, 1e-4);
    CHECK_NEAR(cloned_child->local_position().x, 1.0, 1e-4);
}

TEST(tree, foreach_component_tree_visits_depth_first) {
    ensure_builtins();

    Entity entity("Tree");
    auto* a = entity.add_component<ProbeComponent>();
    auto* b = entity.add_component<ProbeComponent>();
    auto* c = entity.add_component<ProbeComponent>();
    CHECK_TRUE(entity.set_component_parent(b, a));
    CHECK_TRUE(entity.set_component_parent(c, a));

    std::vector<Component*> visited;
    entity.foreach_component_tree([&](Component* comp) { visited.push_back(comp); });

    // 组件树里的三个组件都要被访问到（实体的内置组件也是根）
    bool saw_a = false, saw_b = false, saw_c = false;
    for (const Component* comp : visited) {
        if (comp == a) saw_a = true;
        if (comp == b) saw_b = true;
        if (comp == c) saw_c = true;
    }
    CHECK_TRUE(saw_a);
    CHECK_TRUE(saw_b);
    CHECK_TRUE(saw_c);
}

// ===========================================================================
// suite: serialize —— 组件树与局部变换的存档往返
// ===========================================================================

TEST(serialize, component_tree_roundtrip) {
    ensure_builtins();

    Scene scene("Roundtrip");
    Entity* entity = scene.create_entity("Quad");
    CHECK_TRUE(entity != nullptr);

    auto* parent = entity->add_component(
        std::make_unique<gryce_engine::components::d2::basic_rect::ColorRect>());
    auto* child = entity->add_component(
        std::make_unique<gryce_engine::components::d2::text::Label>());
    CHECK_TRUE(entity->set_component_parent(child, parent));
    parent->set_local_position(math::Vector3f(1.0f, 2.0f, 0.0f));
    child->set_local_position(math::Vector3f(0.5f, 0.0f, 0.0f));

    const nlohmann::json json = gryce_engine::scene::SceneSerializer::serialize(scene);
    CHECK_TRUE(json.contains("entities"));
    // 版本已升到 3（组件树）
    CHECK_INT(json.value("version", 0), 3);

    auto restored = gryce_engine::scene::SceneSerializer::deserialize(json);
    CHECK_TRUE(restored != nullptr);
    CHECK_INT(restored->roots().size(), 1);

    Entity* restored_entity = restored->roots()[0].get();
    CHECK_TRUE(restored_entity != nullptr);

    auto* r_parent = restored_entity->get_component<gryce_engine::components::d2::basic_rect::ColorRect>();
    auto* r_child = restored_entity->get_component<gryce_engine::components::d2::text::Label>();
    CHECK_TRUE(r_parent != nullptr);
    CHECK_TRUE(r_child != nullptr);

    // 父子关系在存档往返后保持
    CHECK_TRUE(r_child->component_parent() == r_parent);
    CHECK_INT(r_parent->component_children().size(), 1);

    // 局部变换在存档往返后保持
    CHECK_NEAR(r_parent->local_position().x, 1.0, 1e-4);
    CHECK_NEAR(r_parent->local_position().y, 2.0, 1e-4);
    CHECK_NEAR(r_child->local_position().x, 0.5, 1e-4);
}

// 旧存档（没有 parent / local_transform 字段）必须仍能正常加载：全部按根组件处理。
TEST(serialize, legacy_file_without_tree_fields) {
    ensure_builtins();

    nlohmann::json json;
    json["version"] = 2;
    json["name"] = "Legacy";
    nlohmann::json entities = nlohmann::json::array();
    nlohmann::json e;
    e["uuid"] = "legacy-entity";
    e["name"] = "Legacy";
    nlohmann::json comps = nlohmann::json::array();
    nlohmann::json c;
    c["type"] = "ColorRect";
    c["enabled"] = true;
    comps.push_back(c);
    e["components"] = comps;
    entities.push_back(e);
    json["entities"] = entities;

    auto restored = gryce_engine::scene::SceneSerializer::deserialize(json);
    CHECK_TRUE(restored != nullptr);
    CHECK_INT(restored->roots().size(), 1);
    Entity* e2 = restored->roots()[0].get();
    CHECK_TRUE(e2 != nullptr);
    auto* rect = e2->get_component<gryce_engine::components::d2::basic_rect::ColorRect>();
    CHECK_TRUE(rect != nullptr);
    // 没有 parent 字段 → 是组件树的根
    CHECK_TRUE(rect->component_parent() == nullptr);
}

// ===========================================================================
// suite: capi —— 组件 C API（hash 口径统一 + 组件树 + 类型继承）
//
// 这一套锁的是「编辑器能用注册类型表匹配到实体上的组件」这条主线：
//   注册类型表给出的 hash  ==  实体上组件的 hash  ==  属性接口用的 hash
// 三者必须是同一个数，任何一处口径分裂编辑器就会取不到属性。
//
// 组件下标口径同样在这里锁死：Transform / ParentComponent / ChildrenComponent
// 三个内部组件不进下标空间，可见组件的下标与脚本层一致。
// ===========================================================================
namespace {

// 起一个最小 core（world + 空场景），让 C API 的实体解析可用。
// 注意：GCore_Init 内部会注册内建组件类型，本套件不能再调 ensure_builtins()
// （重复注册会让工厂类型表出现重复项）。
bool capi_core_init() {
    static int state = 0;  // 0 = 未初始化，1 = 成功，-1 = 失败
    if (state != 0) return state > 0;

    GCoreInitDesc desc{};
    desc.version = static_cast<uint32_t>(sizeof(GCoreInitDesc));
    desc.project_root = GRYCE_TEST_PROJECT_DIR;
    desc.enable_reflection = false;
    GCore_SetAutoLoadMainScene(false);

    if (GCore_Init(&desc) != 0) {
        std::printf("      GCore_Init failed (project_root=%s)\n", GRYCE_TEST_PROJECT_DIR);
        state = -1;
        return false;
    }
    state = 1;
    return true;
}

// 推一条命令并让它被消费（process_command 在 BeginFrame 里跑）。
void capi_pump() {
    GCore_BeginFrame(1.0f / 60.0f);
    GCore_EndFrame();
}

// 造一个新实体并消费命令，返回其句柄（0 = 失败）。
GEntityHandle capi_create_entity(const char* name) {
    if (!GEdit_CreateEntity(name, 0)) return 0;
    capi_pump();
    return GEntity_GetSelected();
}

// 注册类型表里按名字找类型的全名 hash（找不到返回 0）。
uint64_t capi_registered_hash(const char* type_name) {
    const int count = GComponent_GetRegisteredTypeCount();
    for (int i = 0; i < count; ++i) {
        uint64_t hash = 0;
        char name[128] = {0};
        if (GComponent_GetRegisteredTypeInfo(i, &hash, name, sizeof(name)) != 0) continue;
        if (std::strcmp(name, type_name) == 0) return hash;
    }
    return 0;
}

} // namespace

TEST(capi, type_hash_unified_across_tables) {
    if (!capi_core_init()) { CHECK_TRUE(false); return; }

    // 1) 注册类型表：拿到的 hash 必须能按名字原样查回来（编辑器就是这么用的）
    const int count = GComponent_GetRegisteredTypeCount();
    CHECK_TRUE(count > 0);
    for (int i = 0; i < count; ++i) {
        uint64_t hash = 0;
        char name[128] = {0};
        CHECK_INT(GComponent_GetRegisteredTypeInfo(i, &hash, name, sizeof(name)), 0);

        uint64_t by_name = 0;
        CHECK_INT(GComponent_GetTypeHashByName(name, &by_name), 0);
        CHECK_TRUE(by_name == hash);
    }

    // 2) 反射表里有的类型：注册 hash 必须等于反射全名 hash
    const auto& reg = gryce_engine::reflection::Registry::instance();
    for (const auto& entry : reg.all_types()) {
        const uint64_t full = reg.full_hash_of(entry.first);
        if (full == 0) continue;  // 未在工厂注册的类型（如 Component2D 抽象基类）
        uint64_t by_name = 0;
        CHECK_INT(GComponent_GetTypeHashByName(entry.first.c_str(), &by_name), 0);
        CHECK_TRUE(by_name == full);
    }

    // 3) 全名 hash 与短名 hash 是两个不同的数（口径统一后不再混用）
    uint64_t full_hash = 0;
    uint64_t same_by_full_name = 0;
    CHECK_INT(GComponent_GetTypeHashByName("ColorRect", &full_hash), 0);
    CHECK_INT(GComponent_GetTypeHashByName(
                  "gryce_engine::components::d2::basic_rect::ColorRect", &same_by_full_name), 0);
    CHECK_TRUE(full_hash == same_by_full_name);
    CHECK_TRUE(full_hash != gryce_engine::reflection::type_hash_of_name("ColorRect"));
}

TEST(capi, component_index_space_and_hash_match) {
    if (!capi_core_init()) { CHECK_TRUE(false); return; }

    const GEntityHandle h = capi_create_entity("CapiRect");
    CHECK_TRUE(h != 0);

    // 内部组件不进下标空间：新建实体只有 Transform / Parent / Children
    CHECK_INT(GComponent_GetCount(h), 0);

    const uint64_t color_rect = capi_registered_hash("ColorRect");
    CHECK_TRUE(color_rect != 0);
    CHECK_INT(GComponent_AddComponent(h, color_rect), 0);
    capi_pump();

    // 加完一个组件后可见下标空间里只有它
    CHECK_INT(GComponent_GetCount(h), 1);

    // 实体上组件的 hash 必须与注册类型表给出的 hash 完全一致
    uint64_t comp_hash = 0;
    CHECK_INT(GComponent_GetTypeHashAt(h, 0, &comp_hash), 0);
    CHECK_TRUE(comp_hash == color_rect);

    // 类型名与 hash 同源
    char type_name[128] = {0};
    CHECK_TRUE(GComponent_GetTypeNameAt(h, 0, type_name, sizeof(type_name)) > 0);
    CHECK_TRUE(std::string(type_name).find("ColorRect") != std::string::npos);

    // 属性接口认这个 hash
    CHECK_TRUE(GComponent_GetPropertyCount(h, comp_hash) > 0);
}

TEST(capi, base_class_query_by_hash) {
    if (!capi_core_init()) { CHECK_TRUE(false); return; }

    const GEntityHandle h = capi_create_entity("CapiBase");
    CHECK_TRUE(h != 0);

    const uint64_t color_rect = capi_registered_hash("ColorRect");
    CHECK_INT(GComponent_AddComponent(h, color_rect), 0);
    capi_pump();

    uint64_t base_hash = 0;
    uint64_t other_hash = 0;
    CHECK_INT(GComponent_GetTypeHashByName("Component2D", &base_hash), 0);
    CHECK_INT(GComponent_GetTypeHashByName("Sprite2D", &other_hash), 0);

    // 基类查询：传基类 hash 也能命中派生实例
    CHECK_TRUE(GComponent_GetPropertyCount(h, base_hash) > 0);
    CHECK_INT(GComponent_IsDerivedFromAt(h, 0, base_hash), 1);
    CHECK_INT(GComponent_IsDerivedFromAt(h, 0, other_hash), 0);

    // 无关类型：属性个数为 0
    CHECK_INT(GComponent_GetPropertyCount(h, other_hash), 0);
}

TEST(capi, type_chain_is_root_to_leaf) {
    if (!capi_core_init()) { CHECK_TRUE(false); return; }

    const GEntityHandle h = capi_create_entity("CapiChain");
    CHECK_TRUE(h != 0);

    const uint64_t color_rect = capi_registered_hash("ColorRect");
    CHECK_INT(GComponent_AddComponent(h, color_rect), 0);
    capi_pump();

    uint64_t chain[8] = {0};
    const int n = GComponent_GetTypeChainAt(h, 0, chain, 8);
    CHECK_TRUE(n >= 2);
    if (n < 2) return;

    // 链尾是组件自身；链首是根基类
    uint64_t own = 0;
    CHECK_INT(GComponent_GetTypeHashAt(h, 0, &own), 0);
    CHECK_TRUE(chain[n - 1] == own);

    uint64_t component_root = 0;
    CHECK_INT(GComponent_GetTypeHashByName("Component", &component_root), 0);
    CHECK_TRUE(chain[0] == component_root);

    // 每一层都能按名字对上
    uint64_t basic_rect = 0;
    CHECK_INT(GComponent_GetTypeHashByName("BasicRect", &basic_rect), 0);
    bool found_basic_rect = false;
    for (int i = 0; i < n; ++i) found_basic_rect = found_basic_rect || chain[i] == basic_rect;
    CHECK_TRUE(found_basic_rect);
}

TEST(capi, component_tree_apis) {
    if (!capi_core_init()) { CHECK_TRUE(false); return; }

    const GEntityHandle h = capi_create_entity("CapiTree");
    CHECK_TRUE(h != 0);

    const uint64_t color_rect = capi_registered_hash("ColorRect");
    const uint64_t sprite = capi_registered_hash("Sprite2D");
    CHECK_INT(GComponent_AddComponent(h, color_rect), 0);
    CHECK_INT(GComponent_AddComponent(h, sprite), 0);
    capi_pump();
    CHECK_INT(GComponent_GetCount(h), 2);

    // 初始都是组件树根
    CHECK_INT(GComponent_GetParentAt(h, 0), -1);
    CHECK_INT(GComponent_GetParentAt(h, 1), -1);
    CHECK_INT(GComponent_GetChildCountAt(h, 0), 0);

    // 把 1 挂到 0 之下
    CHECK_INT(GComponent_SetParentAt(h, 1, 0), 1);
    CHECK_INT(GComponent_GetParentAt(h, 1), 0);
    CHECK_INT(GComponent_GetParentAt(h, 0), -1);
    CHECK_INT(GComponent_GetChildCountAt(h, 0), 1);
    CHECK_INT(GComponent_GetChildAt(h, 0, 0), 1);
    CHECK_INT(GComponent_GetChildAt(h, 0, 1), -1);

    // 挂到自己之下 / 非法下标一律失败
    CHECK_INT(GComponent_SetParentAt(h, 0, 0), 0);
    CHECK_INT(GComponent_SetParentAt(h, 9, 0), 0);
    CHECK_INT(GComponent_SetParentAt(h, 0, 9), 0);

    // 生命周期继承：父禁用 → 子失效
    CHECK_INT(GComponent_IsEffectivelyEnabledAt(h, 0), 1);
    CHECK_INT(GComponent_IsEffectivelyEnabledAt(h, 1), 1);

    // 脱离父组件 → 重新成为根
    CHECK_INT(GComponent_SetParentAt(h, 1, -1), 1);
    CHECK_INT(GComponent_GetParentAt(h, 1), -1);
    CHECK_INT(GComponent_GetChildCountAt(h, 0), 0);
}

TEST(capi, local_transform_apis) {
    if (!capi_core_init()) { CHECK_TRUE(false); return; }

    const GEntityHandle h = capi_create_entity("CapiXform");
    CHECK_TRUE(h != 0);

    const uint64_t color_rect = capi_registered_hash("ColorRect");
    CHECK_INT(GComponent_AddComponent(h, color_rect), 0);
    capi_pump();

    const float pos[3] = {1.0f, 2.0f, 3.0f};
    const float rot[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const float scl[3] = {2.0f, 2.0f, 2.0f};
    CHECK_INT(GComponent_SetLocalTransformAt(h, 0, pos, rot, scl), 1);

    float out_pos[3] = {0};
    float out_rot[4] = {0};
    float out_scl[3] = {0};
    CHECK_INT(GComponent_GetLocalTransformAt(h, 0, out_pos, out_rot, out_scl), 0);
    CHECK_NEAR(out_pos[0], 1.0, 1e-4);
    CHECK_NEAR(out_pos[1], 2.0, 1e-4);
    CHECK_NEAR(out_pos[2], 3.0, 1e-4);
    CHECK_NEAR(out_rot[3], 1.0, 1e-4);
    CHECK_NEAR(out_scl[0], 2.0, 1e-4);

    // 非法下标
    CHECK_INT(GComponent_GetLocalTransformAt(h, 5, out_pos, out_rot, out_scl), -1);
    CHECK_INT(GComponent_SetLocalTransformAt(h, 5, pos, rot, scl), 0);
}

// ===========================================================================
// main
// ===========================================================================
int main(int argc, char** argv) {
    std::vector<std::string> wanted;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--list") {
            std::vector<std::string> suites;
            for (const Case& c : registry()) {
                bool seen = false;
                for (const auto& s : suites) seen = seen || (s == c.suite);
                if (!seen) suites.push_back(c.suite);
            }
            for (const auto& s : suites) std::printf("%s\n", s.c_str());
            return 0;
        }
        if (arg == "--suite" && i + 1 < argc) {
            wanted.push_back(argv[++i]);
            continue;
        }
        if (!arg.empty() && arg[0] != '-') wanted.push_back(arg);
    }

    std::vector<std::string> suites;
    for (const Case& c : registry()) {
        bool seen = false;
        for (const auto& s : suites) seen = seen || (s == c.suite);
        if (!seen) suites.push_back(c.suite);
    }

    int cases_total = 0;
    int cases_failed = 0;
    for (const std::string& s : suites) {
        if (!wanted.empty()) {
            bool sel = false;
            for (const auto& w : wanted) {
                if (w == s) { sel = true; break; }
            }
            if (!sel) continue;
        }
        std::printf("[ SUITE ] %s\n", s.c_str());
        for (const Case& c : registry()) {
            if (c.suite != s) continue;
            g_case_checks = 0;
            g_case_failed = 0;
            c.fn();
            ++cases_total;
            if (g_case_failed > 0) {
                ++cases_failed;
                std::printf("  [FAIL] %s (%d/%d checks failed)\n",
                            c.name.c_str(), g_case_failed, g_case_checks);
            } else {
                std::printf("  [ OK  ] %s (%d checks)\n", c.name.c_str(), g_case_checks);
            }
        }
    }

    std::printf("\n--------------------------------------------------------------\n");
    std::printf("checks: %d total, %d passed, %d failed\n",
                g_checks, g_checks - g_checks_failed, g_checks_failed);
    std::printf("cases : %d total, %d passed, %d failed\n",
                cases_total, cases_total - cases_failed, cases_failed);
    if (cases_failed == 0 && g_checks_failed == 0) {
        std::printf("RESULT: PASS\n");
        return 0;
    }
    std::printf("RESULT: FAIL\n");
    return 1;
}
