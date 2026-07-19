#pragma once

#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "scene/entity.h"

namespace gryce_engine::components {
class PrefabInstance;
} // namespace gryce_engine::components

namespace gryce_engine::scene {

class Scene;

// ---------------------------------------------------------------------------
// Prefab — 预制体模板。
//
// 预制体文件（.geprefab / .gesc，JSON 格式）描述一棵实体子树；
// 实例化时生成全新的 UUID / EntityID，并在实例根挂载 PrefabInstance 组件
// 记录来源与覆盖参数。场景保存时实例写成紧凑的 prefab 引用，加载时重新
// 实例化 —— 修改预制体文件会自动传播到所有引用它的场景。
//
// 支持嵌套：预制体文件内可包含 prefab 引用节点，实例化时递归展开；
// 循环引用会被检测并拒绝。
//
// 覆盖参数（overrides）JSON 结构：
// {
//   "name": "...",                           // 实例根改名
//   "enabled": false,                        // 实例根开关
//   "transform": { "position": [x,y,z], ... },  // 实例根 Transform 部分覆盖
//   "components": { "Type": { ... } },       // 实例根组件深合并覆盖
//   "entities": {                            // 子实体覆盖，键为模板UUID或实体名
//     "<tpl-uuid-or-name>": {
//       "name": "...", "enabled": true,
//       "transform": { ... },
//       "components": { "Type": { ... } }
//     }
//   },
//   "remove": ["<tpl-uuid-or-name>", ...]    // 从实例中移除模板成员
// }
// ---------------------------------------------------------------------------
class Prefab {
public:
    Prefab() = default;
    ~Prefab() = default;

    Prefab(const Prefab&) = delete;
    Prefab& operator=(const Prefab&) = delete;
    Prefab(Prefab&&) = default;
    Prefab& operator=(Prefab&&) = default;

    // 从 .gesc / .geprefab 文件加载预制体模板（不关联任何 Scene）
    static std::unique_ptr<Prefab> load(const std::string& path);

    // 兼容旧 API：纯拷贝式实例化（不挂 PrefabInstance 标记）。
    // 需要完整实例语义（覆盖/序列化引用）请用静态 instantiate()。
    Entity* instantiate(Scene* scene) const;

    const std::vector<std::unique_ptr<Entity>>& roots() const { return roots_; }

    // --- 完整 Prefab 工作流 ---

    // 把实体子树保存为预制体文件（root 的 parent 引用会被截断）
    static bool save(const Entity* root, const std::string& path);

    // 实例化到场景（自动挂 PrefabInstance），返回实例根
    static Entity* instantiate(Scene* scene, const std::string& prefab_path,
                               const nlohmann::json& overrides = nlohmann::json::object());

    // 构建实例子树（不挂到场景），供序列化器嵌套展开等场景使用
    static std::unique_ptr<Entity> instantiate_tree(
        const std::string& prefab_path,
        const nlohmann::json& overrides = nlohmann::json::object());

    // 对实例根应用覆盖参数（键为模板UUID或实体名）
    static void apply_overrides(Entity* instance_root, const nlohmann::json& overrides);

    // 将实例还原为 模板+overrides 状态。
    // 保留实例根本身的 UUID / 挂载关系 / 运行时添加的子实体。
    static bool revert(Entity* instance_root);

    // 取实体上的 PrefabInstance 组件（非实例根返回 nullptr）
    static components::PrefabInstance* get_instance(Entity* entity);
    static const components::PrefabInstance* get_instance(const Entity* entity);

    // 按当前子树重建 members 映射（模板UUID -> 实例UUID）
    static void refresh_members(Entity* instance_root);

    // 清空模板缓存（预制体文件被外部修改后调用）
    static void clear_cache();

private:
    static std::shared_ptr<Prefab> load_cached(const std::string& path);

    std::vector<std::unique_ptr<Entity>> roots_;
};

} // namespace gryce_engine::scene
