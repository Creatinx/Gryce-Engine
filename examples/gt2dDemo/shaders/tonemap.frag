#version 330 core

in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

uniform sampler2D uHDRTexture;
uniform sampler2D uBloomTexture;
uniform sampler2D uLUTTexture;
uniform sampler2D uExposureTexture;
uniform float uExposure = 1.0;
uniform float uEV100 = -1.0;      // >= 0 时按摄影 EV100 推导曝光
uniform int uToneMapMode = 1;     // 0 none, 1 Reinhard, 2 ACES, 3 AgX, 4 filmic
uniform int uDithering = 1;
uniform int uBloomEnabled = 1;
uniform float uBloomIntensity = 0.35;
uniform int uUseLUT = 0;
uniform float uLUTStrength = 1.0;
uniform int uAutoExposure = 0;
uniform float uFilmGrain = 0.0;
uniform float uVignette = 0.0;
uniform float uChromaticAberration = 0.0;
uniform float uTAAWeight = 0.85;
uniform float uWhitePoint = 1.0;
uniform float uBlackPoint = 0.0;
uniform float uContrast = 1.0;
uniform float uSaturation = 1.0;
uniform vec4 uLift = vec4(0.0);
uniform vec4 uGamma = vec4(1.0);
uniform vec4 uGain = vec4(1.0);
uniform vec4 uShadows = vec4(0.0);
uniform vec4 uMidtones = vec4(0.0);
uniform vec4 uHighlights = vec4(0.0);

float interleaved_gradient_noise(vec2 pixel) {
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

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

// ---------------------------------------------------------------------------
// AgX（Stephen Hill / Blender 采用），相比 ACES 色相偏移更小、暗部更干净
// ---------------------------------------------------------------------------
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

    // 输入变换
    val = agx_mat * val;
    // AgX 默认对比
    val = agx_default_contrast(val);
    // 编码到 log2 空间
    const float min_ev = -12.47393;
    const float max_ev = 4.026069;
    val = clamp((log2(val + 1e-7) - min_ev) / (max_ev - min_ev), 0.0, 1.0);
    // 逆输入变换
    val = agx_mat_inv * val;
    return clamp(val, 0.0, 1.0);
}

// ---------------------------------------------------------------------------
// Filmic 参数化曲线：白点/黑点重映射 + 对比度 + 饱和度
// ---------------------------------------------------------------------------
vec3 filmic_curve(vec3 x) {
    float white = max(uWhitePoint, uBlackPoint + 1e-4);
    x = (x - uBlackPoint) / (white - uBlackPoint);
    x = mix(vec3(0.5), x, uContrast);
    return clamp(x, 0.0, 1.0);
}

// DaVinci 风格 Lift/Gamma/Gain + 阴影/中间调/高光（显示参考空间调色）
vec3 color_grade(vec3 x) {
    x = mix(vec3(0.5), x, uContrast);
    float luma = dot(x, vec3(0.2126, 0.7152, 0.0722));
    x = mix(vec3(luma), x, uSaturation);
    x = pow(max(x, vec3(0.0)), vec3(uGamma.r, uGamma.g, uGamma.b));
    x = x * vec3(uGain.r, uGain.g, uGain.b) + vec3(uLift.r, uLift.g, uLift.b);
    float lum = dot(x, vec3(0.2126, 0.7152, 0.0722));
    float shadows_w = 1.0 - smoothstep(0.0, 0.4, lum);
    float highlights_w = smoothstep(0.6, 1.0, lum);
    float midtones_w = 1.0 - shadows_w - highlights_w;
    x += uShadows.rgb * shadows_w + uMidtones.rgb * midtones_w + uHighlights.rgb * highlights_w;
    return clamp(x, 0.0, 1.0);
}

// 1024x32 打包的 3D LUT：R = 32 格内 x，G = y，B = 格索引，三线性插值
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

void main() {
    // 色差（Chromatic Aberration）：按距画面中心的距离偏移各通道采样
    vec2 dir = vTexCoord - 0.5;
    float ca = uChromaticAberration * length(dir) * 0.02;
    vec3 hdr;
    hdr.r = texture(uHDRTexture, vTexCoord + dir * ca).r;
    hdr.g = texture(uHDRTexture, vTexCoord).g;
    hdr.b = texture(uHDRTexture, vTexCoord - dir * ca).b;
    float exposure = uExposure;
    if (uEV100 >= 0.0) {
        exposure = 1.0 / (1.2 * pow(2.0, uEV100));
    }
    if (uAutoExposure != 0) {
        exposure = texture(uExposureTexture, vec2(0.5)).r;
    }
    hdr *= exposure;

    // Bloom 合成（HDR 空间线性叠加）
    if (uBloomEnabled != 0) {
        hdr += texture(uBloomTexture, vTexCoord).rgb * uBloomIntensity;
    }

    vec3 ldr;
    if (uToneMapMode == 0) {
        ldr = clamp(hdr, 0.0, 1.0);
    } else if (uToneMapMode == 2) {
        ldr = aces_approx(hdr);
    } else if (uToneMapMode == 3) {
        ldr = agx(hdr);
    } else if (uToneMapMode == 4) {
        ldr = filmic_curve(hdr);
    } else {
        ldr = reinhard(hdr);
    }

    // 调色（显示参考空间）
    ldr = color_grade(ldr);

    // 3D LUT 色彩分级
    if (uUseLUT != 0) {
        ldr = mix(ldr, apply_lut(ldr), uLUTStrength);
    }

    // 暗角
    if (uVignette > 0.0) {
        float d = distance(vTexCoord, vec2(0.5));
        ldr *= 1.0 - uVignette * smoothstep(0.35, 0.9, d);
    }

    // gamma correction
    ldr = pow(ldr, vec3(1.0 / 2.2));

    // 胶片颗粒
    if (uFilmGrain > 0.0) {
        float n = interleaved_gradient_noise(gl_FragCoord.xy);
        ldr += (n - 0.5) * uFilmGrain;
    }

    // 8-bit 输出前有序抖动（Bayer 4x4），消除色带
    if (uDithering != 0) {
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
