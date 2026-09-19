#version 450 core

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

layout(binding = 0) uniform sampler2D uHDRTexture;
layout(binding = 4) uniform sampler2D uHistoryTexture;

layout(push_constant) uniform PushConstants {
    float exposure;
    float ev100;
    int mode;
    int dithering;
    float white_point;
    float black_point;
    float contrast;
    float saturation;
    vec4 lift;
    vec4 gamma;
    vec4 gain;
    vec4 shadows;
    vec4 midtones;
    vec4 highlights;
    int bloom_enabled;
    float bloom_threshold;
    float bloom_intensity;
    float film_grain;
    float vignette;
    float chromatic_aberration;
    int use_lut;
    float lut_strength;
    int auto_exposure;
    float ae_target_luminance;
    float ae_min_exposure;
    float ae_max_exposure;
    float ae_speed;
    int taa_enabled;
    float taa_weight;
    float _pad0;
    float _pad1;
} pc;

void main() {
    vec3 current = texture(uHDRTexture, vTexCoord).rgb;
    if (pc.taa_enabled == 0) {
        FragColor = vec4(current, 1.0);
        return;
    }
    vec3 history = texture(uHistoryTexture, vTexCoord).rgb;

    vec2 texel = 1.0 / vec2(textureSize(uHDRTexture, 0));
    vec3 mn = current;
    vec3 mx = current;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec3 c = texture(uHDRTexture, vTexCoord + vec2(float(x), float(y)) * texel).rgb;
            mn = min(mn, c);
            mx = max(mx, c);
        }
    }
    vec3 clamped = clamp(history, mn, mx);
    FragColor = vec4(mix(current, clamped, pc.taa_weight), 1.0);
}
