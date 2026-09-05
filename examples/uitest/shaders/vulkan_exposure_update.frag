#version 450 core

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

layout(binding = 0) uniform sampler2D uTexture;
layout(binding = 4) uniform sampler2D uPrevExposure;

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
    float avg = max(texture(uTexture, vec2(0.5)).r, 1e-4);
    float prev = texture(uPrevExposure, vec2(0.5)).r;
    float target = clamp(pc.ae_target_luminance / avg, pc.ae_min_exposure, pc.ae_max_exposure);
    float exposure = mix(prev, target, clamp(pc.ae_speed, 0.0, 1.0));
    FragColor = vec4(exposure, exposure, exposure, 1.0);
}
