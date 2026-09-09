#version 450 core

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

layout(binding = 0) uniform sampler2D uDepthTexture;

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

float interleaved_gradient_noise(vec2 pixel) {
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

float linearize_depth(float d) {
    // Vulkan NDC z ∈ [0,1]
    return (2.0 * pc.ssao_near * pc.ssao_far) /
           (pc.ssao_far + pc.ssao_near - d * (pc.ssao_far - pc.ssao_near));
}

vec3 reconstruct_view_pos(vec2 uv, float lin) {
    vec2 ndc = uv * 2.0 - 1.0;
    return vec3(ndc.x * pc.ssao_tan_half * pc.ssao_aspect * lin,
                ndc.y * pc.ssao_tan_half * lin,
                -lin);
}

void main() {
    if (pc.ssao_enabled == 0) {
        FragColor = vec4(1.0);
        return;
    }
    float lin = linearize_depth(texture(uDepthTexture, vTexCoord).r);
    if (lin < 0.01 || lin >= pc.ssao_far * 0.99) {
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
            vec2 suv = clamp(vTexCoord + dir * (pc.ssao_radius * t) * texel, 0.001, 0.999);
            float d2 = linearize_depth(texture(uDepthTexture, suv).r);
            if (d2 < 0.01 || d2 >= pc.ssao_far * 0.99) continue;
            vec3 Q = reconstruct_view_pos(suv, d2);
            vec3 diff = Q - P;
            float horizon = diff.z / (length(diff.xy) + 1e-4);
            max_h = max(max_h, horizon);
        }
        if (max_h > -1e2) {
            float sin_h = max_h / sqrt(1.0 + max_h * max_h);
            ao += clamp(sin_h, 0.0, 1.0);
        }
    }
    float result = 1.0 - (ao / 4.0) * pc.ssao_strength;
    FragColor = vec4(vec3(clamp(result, 0.0, 1.0)), 1.0);
}
