#pragma once

#include <memory>
#include <string>
#include <vector>
#include <functional>

#include <nlohmann/json.hpp>

#include "ecs/component_store.h"
#include "scene/entity.h"

namespace gryce_engine::scene {

// ---------------------------------------------------------------------------
// Scene — 场景，包含若干根实体
// ---------------------------------------------------------------------------
class Scene {
public:
    explicit Scene(const std::string& name = "Scene");
    ~Scene();

    const std::string& name() const { return name_; }
    void set_name(const std::string& name) { name_ = name; }

    // 组件存储池访问
    ecs::ComponentStore& component_store() { return component_store_; }
    const ecs::ComponentStore& component_store() const { return component_store_; }

    Entity* create_entity(const std::string& name = "Entity");
    Entity* add_root_entity(std::unique_ptr<Entity> entity);

    // 运行时销毁实体（递归销毁子实体并反注册所有组件）
    bool destroy_entity(Entity* entity);

    // 把另一个 .gesc 场景作为 prefab 实例化到本场景
    // 返回第一个根实体（方便调用方继续操作）
    Entity* create_prefab(const std::string& scene_path);

    // --- 场景差异保存 (Delta Save) ---
    bool has_unsaved_changes() const;
    void mark_saved();
    nlohmann::json serialize_delta() const;
    bool save_delta(const std::string& path);

    // --- 场景热重载 (Hot Reload) ---
    // 重新加载 .gesc 文件，按 UUID 匹配更新现有实体，尽量保留运行时状态
    bool hot_reload(const std::string& path);

    // --- 子场景 / 关卡流送 (Sub-scene / Level Streaming) ---
    struct SubScene {
        std::string path;        // .gesc 文件路径
        std::string label;       // 显示名称
        bool loaded = false;     // 是否已加载
        std::vector<Entity*> entities; // 加载后产生的实体（便于卸载时定位）
    };

    // 流送子场景：加载 .gesc 到本场景，作为一组根实体挂载
    // 返回子场景在列表中的索引，失败返回 -1
    int stream_in(const std::string& path, const std::string& label = "");

    // 卸载子场景：按索引或路径卸载，销毁所有相关实体
    bool stream_out(int index);
    bool stream_out_by_path(const std::string& path);

    const std::vector<SubScene>& sub_scenes() const { return sub_scenes_; }

    const std::vector<std::unique_ptr<Entity>>& roots() const { return roots_; }
    std::vector<std::unique_ptr<Entity>>& roots() { return roots_; }

    Entity* find_entity_by_uuid(const UUID& id);
    Entity* find_entity_by_name(const std::string& name);

    // 遍历所有实体（先根后子）
    void foreach(std::function<void(Entity*)> callback);

    // 场景级生命周期驱动
    void init();
    void update(float dt);
    void render(render::RenderContext& ctx);
    void destroy();

    // 为当前场景根实体及其全部子树设置组件存储池
    //（Undo/Redo 等外部命令恢复实体后需要重新注册到 ECS store）
    void set_store_on_entity_for_all();

private:
    void set_store_on_entity(Entity* entity);
    void clear_all_dirty();
    bool apply_hot_reload_entity(Entity* existing, const nlohmann::json& e_json);

    std::string name_;
    std::vector<std::unique_ptr<Entity>> roots_;
    ecs::ComponentStore component_store_;

    std::vector<SubScene> sub_scenes_;
    nlohmann::json last_saved_snapshot_; // 用于 delta save
};

} // namespace gryce_engine::scene
