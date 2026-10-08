#version 450 core
// ===========================================================================
// vulkan_hiz_reduce.frag — 深度金字塔归约（Vulkan GLSL 方言，2x2 max）
// ===========================================================================
// 与 hiz_reduce.frag（GL 方言）算法完全一致，只换方言：
//   - 版本 450（与其余 vulkan_*.frag 一致）；
//   - 顶点输入是 vec2 pos + vec2 uv（C++ 侧 post_process_ 分支的顶点布局）；
//   - 贴图走 descriptor binding，binding 0 = 后处理"主输入"槽位
//     （VulkanShader::post_process_binding 的默认值），与 GL 的纹理单元 0 对应。
//
// 语义：取 2x2 的 **max**（最远表面），用于 GPU 遮挡剔除。深度为窗口深度 w，
// near→0/far→1、比较函数 LESS（vk_shader.cpp: VK_COMPARE_OP_LESS），对距离单调。
// ---------------------------------------------------------------------------
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

layout(binding = 0) uniform sampler2D uInput;

void main() {
    ivec2 in_size = textureSize(uInput, 0);
    if (in_size.x <= 0 || in_size.y <= 0) {
        FragColor = vec4(1.0);
        return;
    }
    ivec2 dst = ivec2(gl_FragCoord.xy);
    ivec2 base = dst * 2;
    ivec2 max_xy = in_size - ivec2(1);
    ivec2 c00 = clamp(base, ivec2(0), max_xy);
    ivec2 c10 = clamp(base + ivec2(1, 0), ivec2(0), max_xy);
    ivec2 c01 = clamp(base + ivec2(0, 1), ivec2(0), max_xy);
    ivec2 c11 = clamp(base + ivec2(1, 1), ivec2(0), max_xy);

    float d00 = texelFetch(uInput, c00, 0).r;
    float d10 = texelFetch(uInput, c10, 0).r;
    float d01 = texelFetch(uInput, c01, 0).r;
    float d11 = texelFetch(uInput, c11, 0).r;
    FragColor = vec4(max(max(d00, d10), max(d01, d11)));
}
