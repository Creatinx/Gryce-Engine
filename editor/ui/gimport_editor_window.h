#pragma once

#include <string>

#include "../import/gimport_settings.h"

namespace gryce_engine::editor {

// ---------------------------------------------------------------------------
// GImportEditorWindow — 模型导入设置编辑窗口
// ---------------------------------------------------------------------------
// 双击 Project 面板中的 .gimport 文件时打开，允许调整：
//   - scale
//   - generate_collider
//   - add_rigidbody
//   - physics_material
// ---------------------------------------------------------------------------

class GImportEditorWindow {
public:
    void open(const std::string& source_path);
    void draw();
    bool is_open() const { return open_; }

private:
    bool open_ = false;
    std::string source_path_;
    GImportSettings settings_;
    char physics_material_buf_[64] = {};
};

} // namespace gryce_engine::editor
