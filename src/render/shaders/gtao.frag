#version 330 core

in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

uniform sampler2D uDepthTexture;
uniform float uSSAONear;
uniform float uSSAOFar;
uniform float uSSAOTanHalfFov;
uniform float uSSAOAspect;
uniform float uSSAOStrength;
uniform float uSSAORadius;
uniform int uSSAOEnabled;

float interleaved_gradient_noise(vec2 pixel) {
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

float linearize_depth(float d) {
    // 深度纹理里存的是视图变换后的 [0,1] 深度（NDC z 已映射），
    // 直接作为 d01 反算线性深度；不要再做一次 *0.5+0.5（会压缩到 [0.5,1]）。
    float d01 = d;
    return (2.0 * uSSAONear * uSSAOFar) /
           (uSSAOFar + uSSAONear - d01 * (uSSAOFar - uSSAONear));
}

vec3 reconstruct_view_pos(vec2 uv, float lin) {
    vec2 ndc = uv * 2.0 - 1.0;
    return vec3(ndc.x * uSSAOTanHalfFov * uSSAOAspect * lin,
                ndc.y * uSSAOTanHalfFov * lin,
                -lin);
}

void main() {
    if (uSSAOEnabled == 0) {
        FragColor = vec4(1.0);
        return;
    }
    float lin = linearize_depth(texture(uDepthTexture, vTexCoord).r);
    if (lin < 0.01 || lin >= uSSAOFar * 0.99) {
        FragColor = vec4(1.0);
        return;
    }

    vec3 P = reconstruct_view_pos(vTexCoord, lin);
    vec2 texel = 1.0 / vec2(textureSize(uDepthTexture, 0));
    float noise = interleaved_gradient_noise(gl_FragCoord.xy);
    float ao = 0.0;
    for (int d = 0; d < 4; ++d) {
        float ang = noise * 6.2831853 + float(d) * 1.5707963;
        vec2 dir = vec2(cos(ang), sin(ang));
        float max_h = -1e3;
        for (int s = 1; s <= 4; ++s) {
            float t = float(s) / 4.0;
            vec2 suv = clamp(vTexCoord + dir * (uSSAORadius * t) * texel, 0.001, 0.999);
            float d2 = linearize_depth(texture(uDepthTexture, suv).r);
            if (d2 < 0.01 || d2 >= uSSAOFar * 0.99) continue;
            vec3 Q = reconstruct_view_pos(suv, d2);
            vec3 diff = Q - P;
            // 水平角：遮挡物在相机前方（diff.z > 0）时判定为遮挡
            float horizon = diff.z / (length(diff.xy) + 1e-4);
            max_h = max(max_h, horizon);
        }
        if (max_h > -1e2) {
            float sin_h = max_h / sqrt(1.0 + max_h * max_h);
            ao += clamp(sin_h, 0.0, 1.0);
        }
    }
    float result = 1.0 - (ao / 4.0) * uSSAOStrength;
    FragColor = vec4(vec3(clamp(result, 0.0, 1.0)), 1.0);
}
