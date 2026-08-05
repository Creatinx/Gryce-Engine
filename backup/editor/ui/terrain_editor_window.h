#pragma once

#include <string>

#include "components/terrain.h"

namespace gryce_engine {
namespace scene { class Entity; }
namespace editor {

// ---------------------------------------------------------------------------
// TerrainEditorWindow — 地形编辑器基础窗口
//
// 提供地形参数编辑、高度图可视化预览、程序化生成，以及一键生成
// MeshRenderer（导出为项目内临时 OBJ）功能。
// ---------------------------------------------------------------------------
class TerrainEditorWindow {
public:
    TerrainEditorWindow() = default;

    void open(scene::Entity* entity, components::Terrain* terrain);
    void draw();
    bool is_open() const { return open_; }

private:
    bool open_ = false;
    scene::Entity* entity_ = nullptr;
    components::Terrain* terrain_ = nullptr;

    int preview_resolution_ = 64;
    bool apply_on_close_ = false;

    void draw_parameters();
    void draw_heightmap_preview();
    void draw_actions();

    void sync_preview_resolution();
    bool export_mesh_and_create_renderer();
};

} // namespace editor
} // namespace gryce_engine
