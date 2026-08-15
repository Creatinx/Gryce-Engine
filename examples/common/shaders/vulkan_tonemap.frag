#version 450 core

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

layout(binding = 0) uniform sampler2D uHDRTexture;
layout(binding = 1) uniform sampler2D uBloomTexture;
layout(binding = 2) uniform sampler2D uLUTTexture;
layout(binding = 3) uniform sampler2D uExposureTexture;

// 与 C++ VulkanShader::PostProcessPushData 严格对齐（std430，176 字节）
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

vec3 reinhard(vec3 hdr) {
    return hdr / (hdr + vec3(1.0));
}

vec3 aces_approx(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

vec3 agx_default_contrast(vec3 x) {
    const vec3 a = vec3(1.0);
    const vec3 b = vec3(0.0);
    const vec3 c = vec3(1.0);
    const vec3 d = vec3(0.0);
    const vec3 e = vec3(0.0);
    const vec3 f = vec3(0.0);
    return ((x * (a * x + b)) / (x * (c * x + d) + e)) + f;
}

vec3 agx(vec3 val) {
    const mat3 agx_mat = mat3(
         0.842479062253094,  0.0784335999999992, 0.0792237451477643,
         0.0423282422610123, 0.878468636469772,  0.0791661274605434,
         0.0423756549057051, 0.0784336,          0.879142973793104);
    const mat3 agx_mat_inv = mat3(
         1.19687900512017,  -0.0980208811401368, -0.0990297440797204,
        -0.0528968537575735, 1.15190312990417,   -0.0989611768448438,
        -0.0529716359614435, -0.0980434501178918, 1.15107367264116);

    val = agx_mat * val;
    val = agx_default_contrast(val);
    const float min_ev = -12.47393;
    const float max_ev = 4.026069;
    val = clamp((log2(val + 1e-7) - min_ev) / (max_ev - min_ev), 0.0, 1.0);
    val = agx_mat_inv * val;
    return clamp(val, 0.0, 1.0);
}

vec3 filmic_curve(vec3 x) {
    float white = max(pc.white_point, pc.black_point + 1e-4);
    x = (x - pc.black_point) / (white - pc.black_point);
    x = mix(vec3(0.5), x, pc.contrast);
    return clamp(x, 0.0, 1.0);
}

vec3 color_grade(vec3 x) {
    x = mix(vec3(0.5), x, pc.contrast);
    float luma = dot(x, vec3(0.2126, 0.7152, 0.0722));
    x = mix(vec3(luma), x, pc.saturation);
    x = pow(max(x, vec3(0.0)), pc.gamma.rgb);
    x = x * pc.gain.rgb + pc.lift.rgb;
    float lum = dot(x, vec3(0.2126, 0.7152, 0.0722));
    float shadows_w = 1.0 - smoothstep(0.0, 0.4, lum);
    float highlights_w = smoothstep(0.6, 1.0, lum);
    float midtones_w = 1.0 - shadows_w - highlights_w;
    x += pc.shadows.rgb * shadows_w + pc.midtones.rgb * midtones_w + pc.highlights.rgb * highlights_w;
    return clamp(x, 0.0, 1.0);
}

vec3 apply_lut(vec3 color) {
    color = clamp(color, 0.0, 1.0);
    float blue = color.b * 31.0;
    vec2 uv;
    uv.x = (color.r * 31.0 + 0.5) / 32.0;
    uv.x = (floor(blue) + uv.x) / 32.0;
    uv.y = (color.g * 31.0 + 0.5) / 32.0;
    vec3 c0 = texture(uLUTTexture, uv).rgb;
    uv.x += 1.0 / 1024.0;
    vec3 c1 = texture(uLUTTexture, uv).rgb;
    return mix(c0, c1, fract(blue));
}

float interleaved_gradient_noise(vec2 pixel) {
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

void main() {
    vec2 dir = vTexCoord - 0.5;
    float ca = pc.chromatic_aberration * length(dir) * 0.02;
    vec3 hdr;
    hdr.r = texture(uHDRTexture, vTexCoord + dir * ca).r;
    hdr.g = texture(uHDRTexture, vTexCoord).g;
    hdr.b = texture(uHDRTexture, vTexCoord - dir * ca).b;
    float exposure = pc.exposure;
    if (pc.ev100 >= 0.0) {
        exposure = 1.0 / (1.2 * pow(2.0, pc.ev100));
    }
    if (pc.auto_exposure != 0) {
        exposure = texture(uExposureTexture, vec2(0.5)).r;
    }
    hdr *= exposure;

    if (pc.bloom_enabled != 0) {
        hdr += texture(uBloomTexture, vTexCoord).rgb * pc.bloom_intensity;
    }

    vec3 ldr;
    if (pc.mode == 0) {
        ldr = clamp(hdr, 0.0, 1.0);
    } else if (pc.mode == 2) {
        ldr = aces_approx(hdr);
    } else if (pc.mode == 3) {
        ldr = agx(hdr);
    } else if (pc.mode == 4) {
        ldr = filmic_curve(hdr);
    } else {
        ldr = reinhard(hdr);
    }

    ldr = color_grade(ldr);

    if (pc.use_lut != 0) {
        ldr = mix(ldr, apply_lut(ldr), pc.lut_strength);
    }

    if (pc.vignette > 0.0) {
        float d = distance(vTexCoord, vec2(0.5));
        ldr *= 1.0 - pc.vignette * smoothstep(0.35, 0.9, d);
    }

    ldr = pow(ldr, vec3(1.0 / 2.2));

    if (pc.film_grain > 0.0) {
        float n = interleaved_gradient_noise(gl_FragCoord.xy);
        ldr += (n - 0.5) * pc.film_grain;
    }

    if (pc.dithering != 0) {
        const float bayer[16] = float[](
             0.0,  8.0,  2.0, 10.0,
            12.0,  4.0, 14.0,  6.0,
             3.0, 11.0,  1.0,  9.0,
            15.0,  7.0, 13.0,  5.0);
        int ix = int(mod(gl_FragCoord.x, 4.0));
        int iy = int(mod(gl_FragCoord.y, 4.0));
        float b = bayer[ix + iy * 4] / 16.0;
        ldr += (b - 0.5) / 255.0;
    }

    FragColor = vec4(ldr, 1.0);
}
