#version 330 core

in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

uniform sampler2D uTexture;      // AO
uniform sampler2D uDepthTexture;
uniform float uSSAONear;
uniform float uSSAOFar;

float linearize_depth(float d) {
    return (uSSAONear * uSSAOFar) / max(uSSAOFar - d * (uSSAOFar - uSSAONear), 1e-6);
}

void main() {
    // 全分辨率深度纹理的纹素尺寸。采样前统一加 0.25 texel 偏移，与 gtao.frag
    // 里的 depth_uv_offset 保持一致：半分辨率 UV 中心正好落在两个 depth 纹素
    // 中点（Nearest tie），tie-break 由驱动决定，相邻像素可能取到不同深度，
    // 在轮廓处造成双边权重跳变，AO 跨边缘渗漏到物体本身上。
    vec2 depth_texel = 1.0 / vec2(textureSize(uDepthTexture, 0));
    vec2 depth_uv_offset = 0.25 * depth_texel;
    float center_lin = linearize_depth(texture(uDepthTexture, vTexCoord + depth_uv_offset).r);
    vec2 texel = 1.0 / vec2(textureSize(uTexture, 0));
    float acc = 0.0;
    float wsum = 0.0;
    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            vec2 suv = vTexCoord + vec2(float(x), float(y)) * texel;
            float ao = texture(uTexture, suv).r;
            float d = linearize_depth(texture(uDepthTexture, suv + depth_uv_offset).r);
            float depth_w = exp(-abs(d - center_lin) * 0.5);
            float spatial_w = exp(-(float(x * x) + float(y * y)) / 4.0);
            float w = depth_w * spatial_w;
            acc += ao * w;
            wsum += w;
        }
    }
    FragColor = vec4(vec3(acc / max(wsum, 1e-4)), 1.0);
}
