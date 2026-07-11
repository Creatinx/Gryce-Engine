#include "prefab.h"

#include "scene.h"
#include "scene_serializer.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::scene {

std::unique_ptr<Prefab> Prefab::load(const std::string& path) {
    auto scene = SceneSerializer::load_from_file(path);
    if (!scene) {
        GLOG_ERROR("Prefab::load: failed to load prefab from '{}'", path);
        return nullptr;
    }

    auto prefab = std::make_unique<Prefab>();
    // 从加载的 Scene 中移出所有根实体（不触发 Scene 析构时的组件反注册）
    auto& roots = scene->roots();
    for (auto& root : roots) {
        prefab->roots_.push_back(std::move(root));
    }
    roots.clear(); // 清空原 vector，避免 scene 析构时重复释放

    return prefab;
}

Entity* Prefab::instantiate(Scene* scene) const {
    if (!scene || roots_.empty()) return nullptr;

    Entity* first_root = nullptr;
    for (const auto& root : roots_) {
        auto cloned = root->clone();
        Entity* raw = cloned.get();
        if (!first_root) first_root = raw;
        scene->add_root_entity(std::move(cloned));
    }
    return first_root;
}

} // namespace gryce_engine::scene
