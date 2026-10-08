#version 450 core
// ===========================================================================
// vulkan_hiz_reduce.vert — 深度金字塔归约的全屏四边形顶点着色器（Vulkan 方言）
// ===========================================================================
// 与 vulkan_tonemap.vert 同一套约定：vec2 pos + vec2 uv（stride 16），
// 顶点位置即裁剪坐标。归约寻址在片元里用 gl_FragCoord 整数坐标完成。
// ---------------------------------------------------------------------------
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;

layout(location = 0) out vec2 vTexCoord;

void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
