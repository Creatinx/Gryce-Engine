#version 450 core

// [Shader 阶段] Vertex Shader（Vulkan 变体）
// [功能] MSAA 深度 resolve 全屏 pass 的顶点着色器。
//
// 为什么需要这个文件：Vulkan 的 shaderc 生成 SPIR-V 时强制每个用户 in/out
// 都带 location，而 GLSL 330 不允许给 varying 写 location（需要
// ARB_separate_shader_objects / #version 410）。原先 depth_resolve 只有 330 版
// 源码，Vulkan 后端在 resolve_one 里取不到 vulkan_ 前缀的变体就回退到 330 源码，
// shaderc 必然报 "'location' : SPIR-V requires location for user input/output"，
// 回退预编译 .spv 又不存在 → 整个 depth_resolve 加载失败。
// 后果：前向 MSAA pass 写的是多样本深度，单样本 hdr_depth_ 从未被写入，
// 于是 SSR / SSIL / 雾 / 景深在开 MSAA 时全部读到空深度（Vulkan 默认 4x）。

layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;

layout(location = 0) out vec2 vTexCoord;

void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
