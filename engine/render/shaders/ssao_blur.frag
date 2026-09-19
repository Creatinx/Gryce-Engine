#version 330 core

in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

uniform sampler2D uTexture;      // AO
uniform sampler2D uDepthTexture;
uniform float uSSAONear;
uniform float uSSAOFar;

float linearize_depth(float d) {
    // 深度纹理存的是 [0,1] 深度，直接反算线性深度（不再做 *0.5+0.5）。
    float d01 = d;
    return (2.0 * uSSAONear * uSSAOFar) /
           (uSSAOFar + uSSAONear - d01 * (uSSAOFar - uSSAONear));
}

void main() {
    float center_lin = linearize_depth(texture(uDepthTexture, vTexCoord).r);
    vec2 texel = 1.0 / vec2(textureSize(uTexture, 0));
    float acc = 0.0;
    float wsum = 0.0;
    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            vec2 suv = vTexCoord + vec2(float(x), float(y)) * texel;
            float ao = texture(uTexture, suv).r;
            float d = linearize_depth(texture(uDepthTexture, suv).r);
            float depth_w = exp(-abs(d - center_lin) * 0.5);
            float spatial_w = exp(-(float(x * x) + float(y * y)) / 4.0);
            float w = depth_w * spatial_w;
            acc += ao * w;
            wsum += w;
        }
    }
    FragColor = vec4(vec3(acc / max(wsum, 1e-4)), 1.0);
}
