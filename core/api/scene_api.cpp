#include "GryceCore/scene_api.h"
#include "GryceCore/api_guard.h"
#include "internal_state.h"

#include "scene/scene_serializer.h"
#include "scene/scene.h"
#include "ecs/world.h"

using gryce_engine::scene::SceneSerializer;

namespace gc = gryce_core;

extern "C" {

int GScene_Load(const char* path) {
    GRYCE_API_GUARD();
    if (!gc::g_core_state.initialized || !path || !path[0]) return -1;

    auto scene = SceneSerializer::load_from_file(path);
    if (!scene) return -1;

    if (gc::g_core_state.world) {
        gc::g_core_state.world->attach_scene(std::move(scene));
    }
    gc::g_core_state.current_scene_path = path;
    gc::g_core_state.entity_map.rebuild(gc::g_core_state.world->scene());
    gc::g_core_state.selected_entity = 0;
    gc::g_core_state.deferred_entity_list_changed = true;
    gc::g_core_state.deferred_scene_loaded = true;
    return 0;
}

int GScene_Save(const char* path) {
    GRYCE_API_GUARD();
    if (!gc::g_core_state.initialized || !gc::g_core_state.world || !gc::g_core_state.world->scene() || !path) return -1;
    return SceneSerializer::save_to_file(*gc::g_core_state.world->scene(), path) ? 0 : -1;
}

int GScene_GetCurrentPath(char* out_buf, int buf_size) {
    GRYCE_API_GUARD();
    if (!out_buf || buf_size <= 0) return -1;
    std::strncpy(out_buf, gc::g_core_state.current_scene_path.c_str(), static_cast<size_t>(buf_size) - 1);
    out_buf[buf_size - 1] = '\0';
    return static_cast<int>(std::strlen(out_buf));
}

int GScene_New(void) {
    GRYCE_API_GUARD();
    if (!gc::g_core_state.initialized) return -1;
    auto scene = std::make_unique<gryce_engine::scene::Scene>("Untitled");
    if (gc::g_core_state.world) {
        gc::g_core_state.world->attach_scene(std::move(scene));
    }
    gc::g_core_state.current_scene_path.clear();
    gc::g_core_state.entity_map.rebuild(gc::g_core_state.world->scene());
    gc::g_core_state.selected_entity = 0;
    gc::g_core_state.deferred_entity_list_changed = true;
    return 0;
}

} // extern "C"
