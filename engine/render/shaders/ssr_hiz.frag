#version 330 core

in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

// HiZ 构建：2x2 取最小深度下采样。
// 深度缓冲存的是非线性 [0,1] 深度，min 在非线性空间与线性空间等价
// （线性化是单调函数），因此直接对原始值取 min 即可。
uniform sampler2D uInput;   // 上一级：depth_tex（level 0）或 HiZ[i-1]
uniform vec2 uTexelSize;    // 1 / 输入纹理尺寸

void main() {
    vec2 c = vTexCoord;
    float d00 = texture(uInput, c + vec2(-uTexelSize.x, -uTexelSize.y)).r;
    float d10 = texture(uInput, c + vec2( uTexelSize.x, -uTexelSize.y)).r;
    float d01 = texture(uInput, c + vec2(-uTexelSize.x,  uTexelSize.y)).r;
    float d11 = texture(uInput, c + vec2( uTexelSize.x,  uTexelSize.y)).r;
    FragColor = vec4(min(min(d00, d10), min(d01, d11)));
}
