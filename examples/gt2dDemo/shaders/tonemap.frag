#version 330 core

in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

uniform sampler2D uHDRTexture;
uniform float uExposure = 1.0;
uniform int uToneMapMode = 1; // 0: none, 1: Reinhard, 2: ACES approx

vec3 reinhard(vec3 hdr) {
    return hdr / (hdr + vec3(1.0));
}

vec3 aces_approx(vec3 x) {
    // Krzysztof Narkowicz ACES approx
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    vec3 hdr = texture(uHDRTexture, vTexCoord).rgb;
    hdr *= uExposure;

    vec3 ldr;
    if (uToneMapMode == 0) {
        ldr = clamp(hdr, 0.0, 1.0);
    } else if (uToneMapMode == 2) {
        ldr = aces_approx(hdr);
    } else {
        ldr = reinhard(hdr);
    }

    // gamma correction
    ldr = pow(ldr, vec3(1.0 / 2.2));

    FragColor = vec4(ldr, 1.0);
}
