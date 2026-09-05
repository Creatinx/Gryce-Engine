#version 450 core

layout(location = 0) in vec3 vFragPos;
layout(location = 1) in vec2 vTexCoord;
layout(location = 2) in vec3 vColor;
layout(location = 3) in mat3 vTBN;
layout(location = 7) in vec2 vScreenUV;
layout(location = 0) out vec4 FragColor;

#define MAX_LIGHTS 8

struct Light {
    vec4 pos_type;        // xyz=position, w=type (0 方向光 1 点光 2 聚光)
    vec4 dir_range;       // xyz=direction, w=range
    vec4 color_intensity; // xyz=color, w=intensity
    vec4 spot;            // x=cos(outer), y=cos(inner)
};

// 与 C++ VulkanShader::UBOData 严格对齐（std140）
layout(set = 0, binding = 0) uniform MaterialLightUBO {
    vec4 uAlbedoColor;
    vec4 uCameraPos;
    vec4 uEmissiveOpacity; // xyz=emissive, w=opacity
    vec4 uAmbient;
    vec4 uUVTransform;
    float uRoughness;
    float uMetallic;
    float uAO;
    int uUseShadowMap;
    int uUseAlbedoMap;
    int uUseNormalMap;
    int uUseRoughnessMap;
    int uUseMetallicMap;
    int uUseAOMap;
    int uUseEmissiveMap;
    int uHDREnabled;
    int uLightCount;
    int uShadowLightIndex;
    int uUseIBL;
    float uIBLIntensity;
    int uTwoSided;
    vec4 _pad_std140; // 与 C++ UBOData 对齐：uLights 必须落在 160
    Light uLights[MAX_LIGHTS];
    int uCascadeCount;
    int uPCSSEnabled;
    int uDebugMode;
    int _pad;
    vec4 uCascadeSplits;
    vec4 uCascadeBias;
    vec4 uCascadeFarBlend;
    float uPCSSLightSize;
    float uPCSSMaxRadius;
    float uPCSSBlockerScale;
    float _pad2;
    mat4 uCascadeLightSpace[4];
    mat4 uView;
    float uClearcoat;
    float uClearcoatRoughness;
    float uSheen;
    float uAnisotropy;
    float uAnisotropyRotation;
    float _pad3;
    vec4 uSheenTint;
    int uUseSSAO;
    float uSSAOStrength;
    float _pad_ssao0;
    float _pad_ssao1;
} ubo;

layout(set = 0, binding = 1) uniform sampler2D uAlbedoMap;
layout(set = 0, binding = 2) uniform sampler2D uNormalMap;
layout(set = 0, binding = 3) uniform sampler2D uRoughnessMap;
layout(set = 0, binding = 4) uniform sampler2D uMetallicMap;
layout(set = 0, binding = 5) uniform sampler2D uAOMap;
layout(set = 0, binding = 6) uniform sampler2DShadow uShadowMap;
layout(set = 0, binding = 7) uniform sampler2D uEmissiveMap;

layout(set = 0, binding = 9)  uniform samplerCube uIrradianceMap;
layout(set = 0, binding = 10) uniform samplerCube uPrefilterMap;
layout(set = 0, binding = 11) uniform sampler2D uBRDFLUT;

layout(set = 0, binding = 12) uniform sampler2DShadow uShadowMap1;
layout(set = 0, binding = 13) uniform sampler2DShadow uShadowMap2;
layout(set = 0, binding = 14) uniform sampler2DShadow uShadowMap3;
layout(set = 0, binding = 15) uniform sampler2D uShadowMapDepth;
layout(set = 0, binding = 16) uniform sampler2D uShadowMapDepth1;
layout(set = 0, binding = 17) uniform sampler2D uShadowMapDepth2;
layout(set = 0, binding = 18) uniform sampler2D uShadowMapDepth3;
layout(set = 0, binding = 19) uniform sampler2D uSSAOTexture;

const float PI = 3.14159265359;

float distribution_ggx(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    return a2 / (PI * denom * denom + 0.0001);
}

float distribution_ggx_aniso(vec3 N, vec3 H, vec3 T, vec3 B, float ax, float ay) {
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float XdotH = dot(T, H);
    float YdotH = dot(B, H);
    float a2 = ax * ay;
    vec3 v = vec3(ay * XdotH, ax * YdotH, a2 * NdotH);
    float v2 = max(dot(v, v), 1e-6);
    float w2 = a2 / v2;
    return a2 * w2 * w2 / PI;
}

float geometry_schlick_ggx(float NdotV, float roughness) {
    float r = (roughness + 1.0);
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float geometry_smith(vec3 N, vec3 V, vec3 L, float roughness) {
    return geometry_schlick_ggx(max(dot(N, V), 0.0), roughness) *
           geometry_schlick_ggx(max(dot(N, L), 0.0), roughness);
}

vec3 fresnel_schlick(float cos_theta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(1.0 - cos_theta, 5.0);
}

// ---------------------------------------------------------------------------
// Shadow：16-tap Poisson 软阴影 + CSM + PCSS
// ---------------------------------------------------------------------------
const vec2 k_shadow_poisson[16] = vec2[](
    vec2(-0.94201624, -0.39906216), vec2( 0.94558609, -0.76890725),
    vec2(-0.09418410, -0.92938870), vec2( 0.34495938,  0.29387760),
    vec2(-0.91588581,  0.45771432), vec2(-0.81544232, -0.87912464),
    vec2(-0.38277543,  0.27676845), vec2( 0.97484398,  0.75648379),
    vec2( 0.44323325, -0.97511554), vec2( 0.53742981, -0.47373420),
    vec2(-0.26496911, -0.41893023), vec2( 0.79197514,  0.19090188),
    vec2(-0.24188840,  0.99706507), vec2(-0.81409955,  0.91437590),
    vec2( 0.19984126,  0.78641367), vec2( 0.14383161, -0.14100790));

float interleaved_gradient_noise(vec2 pixel) {
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

float shadow_compare(int cascade, vec3 coords) {
    if (cascade == 0) return texture(uShadowMap, coords);
    if (cascade == 1) return texture(uShadowMap1, coords);
    if (cascade == 2) return texture(uShadowMap2, coords);
    return texture(uShadowMap3, coords);
}

vec2 shadow_texel_size(int cascade) {
    if (cascade == 0) return 1.0 / vec2(textureSize(uShadowMap, 0));
    if (cascade == 1) return 1.0 / vec2(textureSize(uShadowMap1, 0));
    if (cascade == 2) return 1.0 / vec2(textureSize(uShadowMap2, 0));
    return 1.0 / vec2(textureSize(uShadowMap3, 0));
}

float shadow_raw_depth(int cascade, vec2 uv) {
    if (cascade == 0) return texture(uShadowMapDepth, uv).r;
    if (cascade == 1) return texture(uShadowMapDepth1, uv).r;
    if (cascade == 2) return texture(uShadowMapDepth2, uv).r;
    return texture(uShadowMapDepth3, uv).r;
}

float cascade_boundary(int i) {
    if (i <= 0) return ubo.uCascadeSplits.x;
    if (i == 1) return ubo.uCascadeSplits.y;
    if (i == 2) return ubo.uCascadeSplits.z;
    if (i == 3) return ubo.uCascadeSplits.w;
    return ubo.uCascadeFarBlend.x;
}

int cascade_from_depth(float depth) {
    int idx = 0;
    if (depth >= ubo.uCascadeSplits.y) idx = 1;
    if (depth >= ubo.uCascadeSplits.z) idx = 2;
    if (depth >= ubo.uCascadeSplits.w) idx = 3;
    return min(idx, max(ubo.uCascadeCount - 1, 0));
}

float slope_bias(int cascade, vec3 normal, vec3 light_dir) {
    float base = (cascade < 3) ? ubo.uCascadeBias[cascade] : ubo.uCascadeBias.w;
    return max(base * (1.0 - dot(normal, light_dir)), base * 0.1);
}

float pcf_cascade(int cascade, vec3 proj_coords, float radius, float bias) {
    vec2 texel = shadow_texel_size(cascade);
    float angle = interleaved_gradient_noise(gl_FragCoord.xy) * 6.2831853;
    float s = sin(angle);
    float c = cos(angle);
    mat2 rot = mat2(c, -s, s, c);
    float lit = 0.0;
    vec3 coords = vec3(proj_coords.xy, proj_coords.z - bias);
    for (int i = 0; i < 16; ++i) {
        vec2 offset = rot * k_shadow_poisson[i];
        lit += shadow_compare(cascade, vec3(coords.xy + offset * texel * radius, coords.z));
    }
    return lit / 16.0;
}

float pcss_cascade(int cascade, vec3 proj_coords, float bias) {
    vec2 texel = shadow_texel_size(cascade);
    float receiver = proj_coords.z;
    float angle = interleaved_gradient_noise(gl_FragCoord.xy) * 6.2831853;
    float s = sin(angle);
    float c = cos(angle);
    mat2 rot = mat2(c, -s, s, c);

    float search_radius = ubo.uPCSSMaxRadius * 0.5;
    float blocker_sum = 0.0;
    float blocker_count = 0.0;
    for (int i = 0; i < 16; ++i) {
        vec2 offset = rot * k_shadow_poisson[i];
        float d = shadow_raw_depth(cascade, proj_coords.xy + offset * texel * search_radius);
        if (d < receiver) {
            blocker_sum += d;
            blocker_count += 1.0;
        }
    }
    if (blocker_count < 1.0) return 1.0;
    float avg_blocker = blocker_sum / blocker_count;
    float penumbra = ubo.uPCSSLightSize * (receiver - avg_blocker) / max(avg_blocker, 1e-4);
    penumbra = clamp(penumbra * ubo.uPCSSBlockerScale, 1.0, ubo.uPCSSMaxRadius);

    float lit = 0.0;
    vec3 coords = vec3(proj_coords.xy, receiver - bias);
    for (int i = 0; i < 16; ++i) {
        vec2 offset = rot * k_shadow_poisson[i];
        lit += shadow_compare(cascade, vec3(coords.xy + offset * texel * penumbra, coords.z));
    }
    return lit / 16.0;
}

// 把 light-space 位置转成 Vulkan 纹理坐标（Z 已由 C++ 重映射为 [0,1]，仅翻转 Y）
vec3 to_shadow_uv(vec4 light_pos) {
    vec3 proj = light_pos.xyz / light_pos.w;
    proj.x = proj.x * 0.5 + 0.5;
    proj.y = 1.0 - (proj.y * 0.5 + 0.5);
    return proj;
}

float cascade_shadow(vec3 frag_pos, vec3 normal, vec3 light_dir, out int out_cascade) {
    out_cascade = 0;
    if (ubo.uUseShadowMap == 0 || ubo.uCascadeCount <= 0) return 1.0;

    vec4 view_pos = ubo.uView * vec4(frag_pos, 1.0);
    float depth = -view_pos.z;
    int cascade = cascade_from_depth(depth);
    out_cascade = cascade;

    vec3 proj = to_shadow_uv(ubo.uCascadeLightSpace[cascade] * vec4(frag_pos, 1.0));
    if (proj.z > 1.0) return 1.0;
    if (proj.x < 0.0 || proj.x > 1.0 || proj.y < 0.0 || proj.y > 1.0) return 1.0;

    float bias = slope_bias(cascade, normal, light_dir);
    float lit = (ubo.uPCSSEnabled != 0)
                    ? pcss_cascade(cascade, proj, bias)
                    : pcf_cascade(cascade, proj, 2.0, bias);

    float edge = min(min(proj.x, 1.0 - proj.x), min(proj.y, 1.0 - proj.y));
    float fade = smoothstep(0.0, 0.05, edge);
    lit = mix(1.0, lit, fade);

    if (cascade < ubo.uCascadeCount - 1) {
        float far_i = cascade_boundary(cascade + 1);
        float next_far = cascade_boundary(cascade + 2);
        float band = max(ubo.uCascadeFarBlend.y * (next_far - far_i), 1e-4);
        float t = clamp((depth - (far_i - band)) / band, 0.0, 1.0);
        if (t > 0.0) {
            int c2 = cascade + 1;
            vec3 p2 = to_shadow_uv(ubo.uCascadeLightSpace[c2] * vec4(frag_pos, 1.0));
            float lit2 = 1.0;
            if (p2.z <= 1.0 && p2.x >= 0.0 && p2.x <= 1.0 && p2.y >= 0.0 && p2.y <= 1.0) {
                float bias2 = slope_bias(c2, normal, light_dir);
                lit2 = (ubo.uPCSSEnabled != 0)
                           ? pcss_cascade(c2, p2, bias2)
                           : pcf_cascade(c2, p2, 2.0, bias2);
            }
            lit = mix(lit, lit2, t);
        }
    }
    return lit;
}

void main() {
    vec2 uv = vTexCoord * ubo.uUVTransform.xy + ubo.uUVTransform.zw;

    vec4 albedo_tex = texture(uAlbedoMap, uv);
    vec3 albedo = ubo.uUseAlbedoMap > 0 ? albedo_tex.rgb : ubo.uAlbedoColor.rgb;
    albedo *= vColor;
    float alpha = (ubo.uUseAlbedoMap > 0 ? albedo_tex.a : 1.0) * ubo.uEmissiveOpacity.w;

    vec3 normal = ubo.uUseNormalMap > 0
        ? normalize(texture(uNormalMap, uv).rgb * 2.0 - 1.0)
        : vec3(0.0, 0.0, 1.0);
    vec3 N = normalize(vTBN * normal);
    if (ubo.uTwoSided != 0 && !gl_FrontFacing) N = -N;

    float roughness = ubo.uUseRoughnessMap > 0 ? texture(uRoughnessMap, uv).r : ubo.uRoughness;
    float metallic = ubo.uUseMetallicMap > 0 ? texture(uMetallicMap, uv).r : ubo.uMetallic;
    float ao = ubo.uUseAOMap > 0 ? texture(uAOMap, uv).r : ubo.uAO;

    vec3 V = normalize(ubo.uCameraPos.xyz - vFragPos);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    vec3 Lo = vec3(0.0);
    float shadow_factor = 1.0;
    int shadow_cascade = 0;
    for (int i = 0; i < ubo.uLightCount; ++i) {
        Light light = ubo.uLights[i];
        int light_type = int(light.pos_type.w + 0.5);
        vec3 L;
        vec3 radiance;
        float shadow = 1.0;

        if (light_type == 0) {
            L = normalize(-light.dir_range.xyz);
            radiance = light.color_intensity.xyz * light.color_intensity.w;
            if (i == ubo.uShadowLightIndex) {
                shadow = cascade_shadow(vFragPos, N, L, shadow_cascade);
                shadow_factor = shadow;
            }
        } else {
            // 点光 / 聚光：inverse-square 衰减 + 平滑截止
            vec3 to_light = light.pos_type.xyz - vFragPos;
            float dist = length(to_light);
            float range = light.dir_range.w;
            if (dist >= range) continue;
            L = to_light / dist;
            float attenuation = 1.0 / max(dist * dist, 1e-3);
            float cutoff = 1.0 - smoothstep(range * 0.7, range, dist);
            radiance = light.color_intensity.xyz * light.color_intensity.w * attenuation * cutoff;

            if (light_type == 2) {
                float cos_angle = dot(-L, normalize(light.dir_range.xyz));
                float spot = smoothstep(light.spot.x, light.spot.y, cos_angle);
                if (spot <= 0.0) continue;
                radiance *= spot;
            }
        }

        vec3 H = normalize(V + L);
        float NDF;
        if (abs(ubo.uAnisotropy) > 0.01) {
            vec3 T = vTBN[0];
            vec3 B = vTBN[1];
            float aspect = sqrt(1.0 - 0.9 * min(abs(ubo.uAnisotropy), 1.0));
            float ax = max(roughness / aspect, 0.01);
            float ay = max(roughness * aspect, 0.01);
            float c = cos(ubo.uAnisotropyRotation);
            float s = sin(ubo.uAnisotropyRotation);
            vec3 Tx = T * c + B * s;
            vec3 Bx = -T * s + B * c;
            NDF = distribution_ggx_aniso(N, H, Tx, Bx, ax, ay);
        } else {
            NDF = distribution_ggx(N, H, roughness);
        }
        float G = geometry_smith(N, V, L, roughness);
        vec3 F = fresnel_schlick(max(dot(H, V), 0.0), F0);

        vec3 kS = F;
        vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);

        vec3 numerator = NDF * G * F;
        float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.0001;
        vec3 specular = numerator / denominator;

        float NdotL = max(dot(N, L), 0.0);
        vec3 brdf = (kD * albedo / PI + specular) * radiance * NdotL * shadow;

        if (ubo.uClearcoat > 0.001) {
            float cr = clamp(ubo.uClearcoatRoughness, 0.02, 1.0);
            float Dc = distribution_ggx(N, H, cr);
            float Gc = geometry_smith(N, V, L, cr);
            float Fc = 0.04 + 0.96 * pow(1.0 - max(dot(H, V), 0.0), 5.0);
            float clearcoat_lobe = Dc * Gc * Fc / (4.0 * max(dot(N, V), 0.0) * NdotL + 0.0001);
            brdf += clearcoat_lobe * radiance * NdotL * shadow * ubo.uClearcoat;
        }
        if (ubo.uSheen > 0.001) {
            float NdotH = max(dot(N, H), 0.0);
            float a = 0.3;
            float Dch = (2.0 + a) * pow(max(1.0 - NdotH * NdotH, 0.0), a * 0.5) / (2.0 * PI);
            vec3 sheen_lobe = ubo.uSheenTint.rgb * ubo.uSheen * Dch * (vec3(1.0) - F0) * NdotL;
            brdf += sheen_lobe * radiance * shadow;
        }

        Lo += brdf;
    }

    vec3 ambient = ubo.uAmbient.rgb * albedo * ao;
    // SSAO 因子：同时作用于平坦环境光与 IBL 环境光。
    // 注意 IBL 分支会整体重写 ambient，因子必须先算好再在两边都乘上，
    // 否则 IBL 开启后 AO 完全不生效。
    float ssao_factor = 1.0;
    if (ubo.uUseSSAO != 0) {
        float ssao = texture(uSSAOTexture, vScreenUV).r;
        ssao_factor = mix(1.0, ssao, ubo.uSSAOStrength);
        ambient *= ssao_factor;
    }
    if (ubo.uUseIBL > 0) {
        vec3 Nsafe = N;
        vec3 irradiance = texture(uIrradianceMap, Nsafe).rgb;
        vec3 diffuse = irradiance * albedo;

        vec3 R = reflect(-V, Nsafe);
        // prefilter cubemap 带 5 级 mip（level 0 ≈ roughness 0，末级 ≈ roughness 1），
        // 按粗糙度选 mip，粗糙表面反射更糊、金属镜面更锐利。
        vec3 prefiltered = textureLod(uPrefilterMap, R, roughness * 4.0).rgb;
        vec2 brdf = texture(uBRDFLUT, vec2(max(dot(Nsafe, V), 0.0), roughness)).rg;
        vec3 F_ibl = fresnel_schlick(max(dot(Nsafe, V), 0.0), F0);
        vec3 specular = prefiltered * (F_ibl * brdf.x + brdf.y);

        vec3 kD = (vec3(1.0) - F_ibl) * (1.0 - metallic);
        ambient = (kD * diffuse + specular) * ao * ubo.uIBLIntensity * ssao_factor;
    }
    vec3 emissive = ubo.uEmissiveOpacity.xyz * (ubo.uUseEmissiveMap > 0 ? texture(uEmissiveMap, uv).rgb : vec3(1.0));

    if (ubo.uDebugMode == 1) {
        FragColor = vec4(albedo, 1.0);
        return;
    }
    if (ubo.uDebugMode == 2) {
        FragColor = vec4(N * 0.5 + 0.5, 1.0);
        return;
    }
    if (ubo.uDebugMode == 3) {
        FragColor = vec4(vec3(roughness), 1.0);
        return;
    }
    if (ubo.uDebugMode == 4) {
        FragColor = vec4(vec3(metallic), 1.0);
        return;
    }
    if (ubo.uDebugMode == 5) {
        FragColor = vec4(vec3(shadow_factor), 1.0);
        return;
    }
    if (ubo.uDebugMode == 6) {
        FragColor = vec4(Lo, 1.0);
        return;
    }
    if (ubo.uDebugMode == 7) {
        FragColor = vec4(ambient, 1.0);
        return;
    }
    if (ubo.uDebugMode == 8) {
        vec3 cascade_color = vec3(0.0);
        if (shadow_cascade == 0) cascade_color = vec3(1.0, 0.0, 0.0);
        else if (shadow_cascade == 1) cascade_color = vec3(0.0, 1.0, 0.0);
        else if (shadow_cascade == 2) cascade_color = vec3(0.0, 0.3, 1.0);
        else cascade_color = vec3(1.0, 0.8, 0.0);
        FragColor = vec4(cascade_color, 1.0);
        return;
    }

    vec3 color = ambient + Lo + emissive;

    if (ubo.uHDREnabled == 0) {
        color = color / (color + vec3(1.0));
        color = pow(color, vec3(1.0 / 2.2));
    }

    FragColor = vec4(color, alpha);
}
