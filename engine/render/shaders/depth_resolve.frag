#version 330 core

// MSAA 深度 resolve（仅 D3D12 需要）：OpenGL 用 glBlitFramebuffer 的
// GL_DEPTH_BUFFER_BIT、Vulkan 的深度保持单样本无需 resolve；D3D12 没有深度
// ResolveSubresource，且强制 DSV/RTV 采样数一致，所以前向 MSAA pass 必须挂
// 多样本深度，pass 结束后用本 shader 逐样本平均写回单样本 hdr_depth_。
//
// 只写深度、不写颜色（depth-only pass：null RTV + DSV）。
// SPIRV-Cross 会把 sampler2DMS/texelFetch 翻成 HLSL Texture2DMS/Load，
// gl_FragDepth 翻成 SV_Depth。

in vec2 vTexCoord;

uniform sampler2DMS uMsDepth;
uniform int uSamples = 4;

void main() {
    // gl_FragCoord 是像素中心（x+0.5），取整即该像素索引。
    ivec2 coord = ivec2(gl_FragCoord.xy);
    float sum = 0.0;
    for (int i = 0; i < uSamples; ++i) {
        sum += texelFetch(uMsDepth, coord, i).r;
    }
    gl_FragDepth = sum / float(uSamples);
}
