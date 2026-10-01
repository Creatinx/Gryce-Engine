#version 330 core

in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

// HiZ 构建：对输入纹理做精确的 2x2 最小值下采样（OpenGL 版）。
//
// 用 texelFetch + 整数纹素坐标，而不是 texture() + 半纹素偏移：
//   - 纹理过滤/环绕模式完全不参与计算，min 结果由纹素索引唯一确定，
//     GL 与 Vulkan 必然得到逐像素相同的金字塔（旧写法在纹素边界上采样，
//     线性过滤会把相邻纹素平均掉，两端结果不一致 → 第一阶段跳过行为不同 →
//     反射内容两个后端不一样）。
//   - 不需要 CPU 下发 uTexelSize，尺寸直接从 textureSize 取。
// 深度是非线性的，但线性化是单调函数，因此非线性空间取 min 与线性空间等价。

uniform sampler2D uInput; // 上一级：深度（level 0）或 HiZ[i-1]

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
    FragColor = vec4(min(min(d00, d10), min(d01, d11)));
}
