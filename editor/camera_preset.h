#pragma once

namespace gryce_engine::editor {

// ---------------------------------------------------------------------------
// CameraPreset — 编辑器相机预设
// ---------------------------------------------------------------------------
enum class CameraPreset {
    Static,     // 固定最佳视角
    Orbit,      // 自动绕场景旋转
    Flythrough, // 自由飞行
    Demo,       // 播放场景内置相机动画
};

} // namespace gryce_engine::editor
