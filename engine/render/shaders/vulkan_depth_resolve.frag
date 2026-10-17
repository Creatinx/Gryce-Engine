#version 450 core

// [Shader 阶段] Fragment Shader（Vulkan 变体）
// [功能] MSAA 深度 resolve：逐样本平均，把多样本深度写回单样本 hdr_depth_。
//
// 采样器绑定 0 = 后处理描述符集的"主输入"槽位（TextureSlots::kTonemapHDR），
// 与 vulkan_ssr_trace / vulkan_hdr_copy 一致。
//
// 样本数用 textureSamples() 从图像本身查询（OpImageQuerySamples），不再走
// C++ 下发的 uSamples：Vulkan 后处理 shader 的标量统一走 push_constant 块，
// 而 depth_resolve 用的是 General 块（不含 samples 字段），set_uniform_int
// 在 Vulkan 侧会落空。查询图像元数据既省掉一次 CPU 下发，也不会与实际
// 附件采样数不一致。

layout(location = 0) in vec2 vTexCoord;
layout(binding = 0) uniform sampler2DMS uMsDepth;

void main() {
    // gl_FragCoord 是像素中心（x+0.5），取整即该像素索引。
    ivec2 coord = ivec2(gl_FragCoord.xy);
    const int n = textureSamples(uMsDepth);
    float sum = 0.0;
    for (int i = 0; i < n; ++i) {
        sum += texelFetch(uMsDepth, coord, i).r;
    }
    gl_FragDepth = sum / float(n);
}
