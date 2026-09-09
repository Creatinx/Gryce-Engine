#version 450 core

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

layout(binding = 0) uniform sampler2D uTexture;
layout(binding = 4) uniform sampler2D uDepthTexture;

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
    int ssao_enabled;
    float ssao_strength;
    float ssao_radius;
    float ssao_near;
    float ssao_far;
    float ssao_tan_half;
    float ssao_aspect;
    float _pad0;
    float _pad1;
} pc;

float linearize_depth(float d) {
    return (2.0 * pc.ssao_near * pc.ssao_far) /
           (pc.ssao_far + pc.ssao_near - d * (pc.ssao_far - pc.ssao_near));
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
