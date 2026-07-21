#pragma once

#include <string>
#include <functional>

#include "render/material.h"

namespace gryce_engine {
namespace scene { class Entity; }
namespace editor {

// ---------------------------------------------------------------------------
// MaterialEditorWindow — 独立材质编辑窗口
//
// 支持两种编辑模式：
// 1. 编辑实体上的材质（绑定到 MeshRenderer/SkinnedMeshRenderer 的 material）
// 2. 编辑独立的 .gmat 材质资源文件
//
// 打开窗口时传入目标材质指针；关闭时自动尝试保存到关联的 .gmat 路径。
// ---------------------------------------------------------------------------
class MaterialEditorWindow {
public:
    using SaveCallback = std::function<void(render::Material*)>;

    MaterialEditorWindow() = default;

    // 打开并编辑指定材质；on_save 在点击 Save 时被调用
    void open(render::Material* material, scene::Entity* owner = nullptr,
              const std::string& asset_path = "", SaveCallback on_save = nullptr);

    void draw();
    bool is_open() const { return open_; }

    // 当前编辑的材质名称（用于窗口标题）
    std::string title() const;

private:
    bool open_ = false;
    render::Material* material_ = nullptr;
    scene::Entity* owner_ = nullptr;
    std::string asset_path_;
    SaveCallback on_save_;

    // 临时缓冲区（避免每帧重新分配）
    char name_buf_[128] = {};
    char albedo_buf_[256] = {};
    char normal_buf_[256] = {};
    char roughness_buf_[256] = {};
    char metallic_buf_[256] = {};
    char ao_buf_[256] = {};
    char emissive_buf_[256] = {};

    void sync_buffers_from_material();
    void save_material();
};

} // namespace editor
} // namespace gryce_engine
