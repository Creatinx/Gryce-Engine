#version 330 core
// ===========================================================================
// hiz_reduce.vert — 深度金字塔归约的全屏四边形顶点着色器（OpenGL 方言）
// ===========================================================================
// 与 tonemap / gtao 等后处理共用同一套全屏四边形（vec2 pos + vec2 uv，stride 16），
// 顶点位置直接作为裁剪坐标，uv 透传给片元。注意归约本身用 gl_FragCoord 整数寻址，
// 不依赖 vTexCoord —— 这样金字塔的纹素索引完全由帧缓冲像素坐标决定，与过滤/
// 环绕设置无关，GL 与 Vulkan 必然逐像素一致。
// ---------------------------------------------------------------------------
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;

out vec2 vTexCoord;

void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
