#version 330 core

in vec3 vFragPos;
in vec2 vTexCoord;
in vec3 vColor;
in mat3 vTBN;
in vec2 vScreenUV;
out vec4 FragColor;

// ---------------------------------------------------------------------------
// 材质参数
// ---------------------------------------------------------------------------
uniform vec3 uAlbedoColor;
uniform float uRoughness;
uniform float uMetallic;
uniform float uAO;
uniform vec3 uEmissiveColor;
uniform float uOpacity;
uniform vec4 uUVTransform; // xy=scale, zw=offset

// 高级材质
uniform float uClearcoat;
uniform float uClearcoatRoughness;
uniform float uSheen;
uniform vec3 uSheenTint;
uniform float uAnisotropy;
uniform float uAnisotropyRotation;

// 材质纹理
uniform sampler2D uAlbedoMap;
uniform sampler2D uNormalMap;
uniform sampler2D uRoughnessMap;
uniform sampler2D uMetallicMap;
uniform sampler2D uAOMap;
uniform sampler2D uEmissiveMap;

uniform int uUseAlbedoMap;
uniform int uUseNormalMap;
uniform int uUseRoughnessMap;
uniform int uUseMetallicMap;
uniform int uUseAOMap;
uniform int uUseEmissiveMap;
uniform int uTwoSided;

// IBL
uniform samplerCube uIrradianceMap;
uniform samplerCube uPrefilterMap;
uniform sampler2D uBRDFLUT;
uniform int uUseIBL;
uniform float uIBLIntensity;

// SSAO
uniform sampler2D uSSAOTexture;
uniform int uUseSSAO;
uniform float uSSAOStrength;

// ---------------------------------------------------------------------------
// 光照 / 相机 / Cluster 光照
// ---------------------------------------------------------------------------
#define MAX_LIGHTS 64

uniform vec3 uCameraPos;
uniform vec3 uAmbient;
uniform mat4 uViewMatrix;
uniform mat4 uProjectionMatrix;

// Cluster 剪裁后的光源列表（最多 64 个，由 CPU 端 _set_light_uniforms 设置）
uniform int uLightCount;
uniform int uLightType[MAX_LIGHTS];
uniform vec3 uLightPos[MAX_LIGHTS];
uniform vec3 uLightDir[MAX_LIGHTS];
uniform vec3 uLightColor[MAX_LIGHTS];
uniform float uLightIntensity[MAX_LIGHTS];
uniform vec4 uLightParams[MAX_LIGHTS]; // x=range, y=cos(outer), z=cos(inner), w=spot_softness

// ---------------------------------------------------------------------------
// CSM 级联阴影（方向光）
// ---------------------------------------------------------------------------
uniform int uUseShadowMap;
uniform int uShadowLightIndex; // 产生阴影的方向光下标，-1 表示无
uniform sampler2DShadow uShadowMap;
uniform sampler2DShadow uShadowMap1;
uniform sampler2DShadow uShadowMap2;
uniform sampler2DShadow uShadowMap3;
uniform sampler2D uShadowMapDepth;
uniform sampler2D uShadowMapDepth1;
uniform sampler2D uShadowMapDepth2;
uniform sampler2D uShadowMapDepth3;

uniform int uCascadeCount;
uniform vec4 uCascadeSplits;
uniform vec4 uCascadeFarBlend;
uniform vec4 uCascadeBias;
uniform mat4 uCascadeLightSpace[4];

// PCSS
uniform int uPCSSEnabled;
uniform float uPCSSLightSize;
uniform float uPCSSMaxRadius;
uniform float uPCSSBlockerScale;

// ---------------------------------------------------------------------------
// 聚光灯阴影贴图（最多 4 个）
// ---------------------------------------------------------------------------
uniform int uSpotShadowCount;
uniform sampler2DShadow uSpotShadowMap0;
uniform sampler2DShadow uSpotShadowMap1;
uniform sampler2DShadow uSpotShadowMap2;
uniform sampler2DShadow uSpotShadowMap3;
uniform mat4 uSpotLightSpace[4];

// ---------------------------------------------------------------------------
// 调试
// ---------------------------------------------------------------------------
uniform int uDebugMode; // 0 Final, 1 Albedo, 2 Normal, 3 Roughness,
                        // 4 Metallic, 5 Shadow, 6 Direct, 7 Indirect, 8 Cascade

const float PI = 3.14159265359;

// ===========================================================================
// PBR 函数
// ===========================================================================
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

// ===========================================================================
// CSM 阴影采样（与原有 pbr.frag 一致）
// ===========================================================================
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
    if (i <= 0) return uCascadeSplits.x;
    if (i == 1) return uCascadeSplits.y;
    if (i == 2) return uCascadeSplits.z;
    if (i == 3) return uCascadeSplits.w;
    return uCascadeFarBlend.x;
}

int cascade_from_depth(float depth) {
    int idx = 0;
    if (depth >= uCascadeSplits.y) idx = 1;
    if (depth >= uCascadeSplits.z) idx = 2;
    if (depth >= uCascadeSplits.w) idx = 3;
    return min(idx, max(uCascadeCount - 1, 0));
}

float slope_bias(int cascade, vec3 normal, vec3 light_dir) {
    float base = (cascade < 3) ? uCascadeBias[cascade] : uCascadeBias.w;
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

float pcss_cascade(int cascade, vec3 proj_coords, float bias, float normal_dot_light) {
    vec2 texel = shadow_texel_size(cascade);
    float receiver = proj_coords.z;
    float angle = interleaved_gradient_noise(gl_FragCoord.xy) * 6.2831853;
    float s = sin(angle);
    float c = cos(angle);
    mat2 rot = mat2(c, -s, s, c);

    // Blocker search
    float search_radius = uPCSSMaxRadius * 0.5;
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

    // Penumbra
    float penumbra = uPCSSLightSize * (receiver - avg_blocker) / max(avg_blocker, 1e-4);
    penumbra = clamp(penumbra * uPCSSBlockerScale, 1.0, uPCSSMaxRadius);

    // PCF with dynamic radius
    float lit = 0.0;
    vec3 coords = vec3(proj_coords.xy, receiver - bias);
    for (int i = 0; i < 16; ++i) {
        vec2 offset = rot * k_shadow_poisson[i];
        lit += shadow_compare(cascade, vec3(coords.xy + offset * texel * penumbra, coords.z));
    }
    return lit / 16.0;
}

float cascade_shadow(vec3 frag_pos, vec3 normal, vec3 light_dir, out int out_cascade) {
    out_cascade = 0;
    if (uUseShadowMap == 0 || uCascadeCount <= 0) return 1.0;

    vec4 view_pos = uViewMatrix * vec4(frag_pos, 1.0);
    float depth = -view_pos.z;
    int cascade = cascade_from_depth(depth);
    out_cascade = cascade;

    vec4 light_pos = uCascadeLightSpace[cascade] * vec4(frag_pos, 1.0);
    vec3 proj = light_pos.xyz / light_pos.w;
    proj = proj * 0.5 + 0.5;

    if (proj.z > 1.0) return 1.0;
    if (proj.x < 0.0 || proj.x > 1.0 || proj.y < 0.0 || proj.y > 1.0) return 1.0;

    float bias = slope_bias(cascade, normal, light_dir);
    float lit = (uPCSSEnabled != 0)
                    ? pcss_cascade(cascade, proj, bias, dot(normal, light_dir))
                    : pcf_cascade(cascade, proj, 2.0, bias);

    // 阴影贴图边缘淡出
    float edge = min(min(proj.x, 1.0 - proj.x), min(proj.y, 1.0 - proj.y));
    float fade = smoothstep(0.0, 0.05, edge);
    lit = mix(1.0, lit, fade);

    // Cascade Blend
    if (cascade < uCascadeCount - 1) {
        float far_i = cascade_boundary(cascade + 1);
        float next_far = cascade_boundary(cascade + 2);
        float band = max(uCascadeFarBlend.y * (next_far - far_i), 1e-4);
        float t = clamp((depth - (far_i - band)) / band, 0.0, 1.0);
        if (t > 0.0) {
            int c2 = cascade + 1;
            vec4 lp2 = uCascadeLightSpace[c2] * vec4(frag_pos, 1.0);
            vec3 p2 = lp2.xyz / lp2.w;
            p2 = p2 * 0.5 + 0.5;
            float lit2 = 1.0;
            if (p2.z <= 1.0 && p2.x >= 0.0 && p2.x <= 1.0 && p2.y >= 0.0 && p2.y <= 1.0) {
                float bias2 = slope_bias(c2, normal, light_dir);
                lit2 = (uPCSSEnabled != 0)
                           ? pcss_cascade(c2, p2, bias2, dot(normal, light_dir))
                           : pcf_cascade(c2, p2, 2.0, bias2);
            }
            lit = mix(lit, lit2, t);
        }
    }
    return lit;
}

// ===========================================================================
// 聚光灯阴影采样
// ===========================================================================
float spot_shadow_sample(int index, vec3 frag_pos, vec3 normal, vec3 light_dir) {
    if (index >= uSpotShadowCount) return 1.0;

    mat4 light_space = uSpotLightSpace[index];
    vec4 light_pos = light_space * vec4(frag_pos, 1.0);
    vec3 proj = light_pos.xyz / light_pos.w;
    proj = proj * 0.5 + 0.5;

    if (proj.z > 1.0 || proj.z < 0.0) return 1.0;
    if (proj.x < 0.0 || proj.x > 1.0 || proj.y < 0.0 || proj.y > 1.0) return 1.0;

    float bias = max(0.001 * (1.0 - dot(normal, light_dir)), 0.0001);

    if (index == 0) return texture(uSpotShadowMap0, vec3(proj.xy, proj.z - bias));
    if (index == 1) return texture(uSpotShadowMap1, vec3(proj.xy, proj.z - bias));
    if (index == 2) return texture(uSpotShadowMap2, vec3(proj.xy, proj.z - bias));
    if (index == 3) return texture(uSpotShadowMap3, vec3(proj.xy, proj.z - bias));
    return 1.0;
}

// ===========================================================================
// 主函数
// ===========================================================================
void main() {
    vec2 uv = vTexCoord * uUVTransform.xy + uUVTransform.zw;

    // 材质采样
    vec4 albedo_tex = texture(uAlbedoMap, uv);
    vec3 albedo = uUseAlbedoMap > 0 ? albedo_tex.rgb : uAlbedoColor;
    albedo *= vColor;
    float alpha = (uUseAlbedoMap > 0 ? albedo_tex.a : 1.0) * uOpacity;

    vec3 normal = uUseNormalMap > 0
        ? normalize(texture(uNormalMap, uv).rgb * 2.0 - 1.0)
        : vec3(0.0, 0.0, 1.0);
    vec3 N = normalize(vTBN * normal);
    if (uTwoSided != 0 && !gl_FrontFacing) N = -N;

    float roughness = uUseRoughnessMap > 0 ? texture(uRoughnessMap, uv).r : uRoughness;
    float metallic = uUseMetallicMap > 0 ? texture(uMetallicMap, uv).r : uMetallic;
    float ao = uUseAOMap > 0 ? texture(uAOMap, uv).r : uAO;

    vec3 V = normalize(uCameraPos - vFragPos);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    // -----------------------------------------------------------------------
    // 光照累积
    // -----------------------------------------------------------------------
    vec3 Lo = vec3(0.0);
    float shadow_factor = 1.0;
    int shadow_cascade = 0;

    // 跟踪当前处理到的聚光灯阴影索引
    int spot_shadow_index = 0;

    for (int i = 0; i < uLightCount; ++i) {
        vec3 L;
        vec3 radiance;
        float shadow = 1.0;

        if (uLightType[i] == 0) {
            // ---- 方向光 ----
            L = normalize(-uLightDir[i]);
            radiance = uLightColor[i] * uLightIntensity[i];
            if (i == uShadowLightIndex) {
                shadow = cascade_shadow(vFragPos, N, L, shadow_cascade);
                shadow_factor = shadow;
            }
        } else if (uLightType[i] == 1) {
            // ---- 点光：物理正确的 inverse-square 衰减 ----
            vec3 to_light = uLightPos[i] - vFragPos;
            float dist = length(to_light);
            float range = uLightParams[i].x;
            if (dist >= range) continue;
            L = to_light / dist;
            float attenuation = 1.0 / max(dist * dist, 1e-3);
            float cutoff = 1.0 - smoothstep(range * 0.7, range, dist);
            radiance = uLightColor[i] * uLightIntensity[i] * attenuation * cutoff;
        } else if (uLightType[i] == 2) {
            // ---- 聚光灯 ----
            vec3 to_light = uLightPos[i] - vFragPos;
            float dist = length(to_light);
            float range = uLightParams[i].x;
            if (dist >= range) continue;
            L = to_light / dist;
            float attenuation = 1.0 / max(dist * dist, 1e-3);
            float cutoff = 1.0 - smoothstep(range * 0.7, range, dist);
            radiance = uLightColor[i] * uLightIntensity[i] * attenuation * cutoff;

            // 聚光锥形衰减
            float cos_angle = dot(-L, normalize(uLightDir[i]));
            float cos_outer = uLightParams[i].y;
            float cos_inner = uLightParams[i].z;
            float spot = smoothstep(cos_outer, cos_inner, cos_angle);
            if (spot <= 0.0) continue;
            radiance *= spot;

            // 聚光灯阴影
            shadow = spot_shadow_sample(spot_shadow_index, vFragPos, N, L);
            spot_shadow_index++;
        }

        // ---- PBR BRDF ----
        vec3 H = normalize(V + L);
        float NDF;
        if (abs(uAnisotropy) > 0.01) {
            vec3 T = vTBN[0];
            vec3 B = vTBN[1];
            float aspect = sqrt(1.0 - 0.9 * min(abs(uAnisotropy), 1.0));
            float ax = max(roughness / aspect, 0.01);
            float ay = max(roughness * aspect, 0.01);
            float c = cos(uAnisotropyRotation);
            float s = sin(uAnisotropyRotation);
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

        // Clearcoat
        if (uClearcoat > 0.001) {
            float cr = clamp(uClearcoatRoughness, 0.02, 1.0);
            float Dc = distribution_ggx(N, H, cr);
            float Gc = geometry_smith(N, V, L, cr);
            float Fc = 0.04 + 0.96 * pow(1.0 - max(dot(H, V), 0.0), 5.0);
            float clearcoat_lobe = Dc * Gc * Fc / (4.0 * max(dot(N, V), 0.0) * NdotL + 0.0001);
            brdf += clearcoat_lobe * radiance * NdotL * shadow * uClearcoat;
        }
        // Sheen
        if (uSheen > 0.001) {
            float NdotH = max(dot(N, H), 0.0);
            float a = 0.3;
            float Dch = (2.0 + a) * pow(max(1.0 - NdotH * NdotH, 0.0), a * 0.5) / (2.0 * PI);
            vec3 sheen_lobe = uSheenTint * uSheen * Dch * (vec3(1.0) - F0) * NdotL;
            brdf += sheen_lobe * radiance * shadow;
        }

        Lo += brdf;
    }

    // -----------------------------------------------------------------------
    // 环境光 / IBL
    // -----------------------------------------------------------------------
    vec3 ambient = uAmbient * albedo * ao;
    float ssao_factor = 1.0;
    if (uUseSSAO != 0) {
        float ssao = texture(uSSAOTexture, vScreenUV).r;
        ssao_factor = mix(1.0, ssao, uSSAOStrength);
        ambient *= ssao_factor;
    }
    if (uUseIBL > 0) {
        vec3 Nsafe = N;
        vec3 irradiance = texture(uIrradianceMap, Nsafe).rgb;
        vec3 diffuse = irradiance * albedo;

        vec3 R = reflect(-V, Nsafe);
        vec3 prefiltered = textureLod(uPrefilterMap, R, roughness * 4.0).rgb;
        vec2 brdf = texture(uBRDFLUT, vec2(max(dot(Nsafe, V), 0.0), roughness)).rg;
        vec3 F_ibl = fresnel_schlick(max(dot(Nsafe, V), 0.0), F0);
        vec3 specular = prefiltered * (F_ibl * brdf.x + brdf.y);

        vec3 kD = (vec3(1.0) - F_ibl) * (1.0 - metallic);
        ambient = (kD * diffuse + specular) * ao * uIBLIntensity * ssao_factor;
    }
    vec3 emissive = uEmissiveColor * (uUseEmissiveMap > 0 ? texture(uEmissiveMap, uv).rgb : vec3(1.0));

    // -----------------------------------------------------------------------
    // 调试
    // -----------------------------------------------------------------------
    if (uDebugMode == 1) { FragColor = vec4(albedo, 1.0); return; }
    if (uDebugMode == 2) { FragColor = vec4(N * 0.5 + 0.5, 1.0); return; }
    if (uDebugMode == 3) { FragColor = vec4(vec3(roughness), 1.0); return; }
    if (uDebugMode == 4) { FragColor = vec4(vec3(metallic), 1.0); return; }
    if (uDebugMode == 5) { FragColor = vec4(vec3(shadow_factor), 1.0); return; }
    if (uDebugMode == 6) { FragColor = vec4(Lo, 1.0); return; }
    if (uDebugMode == 7) { FragColor = vec4(ambient, 1.0); return; }
    if (uDebugMode == 8) {
        vec3 cascade_color = vec3(0.0);
        if (shadow_cascade == 0) cascade_color = vec3(1.0, 0.0, 0.0);
        else if (shadow_cascade == 1) cascade_color = vec3(0.0, 1.0, 0.0);
        else if (shadow_cascade == 2) cascade_color = vec3(0.0, 0.3, 1.0);
        else cascade_color = vec3(1.0, 0.8, 0.0);
        FragColor = vec4(cascade_color, 1.0);
        return;
    }

    vec3 color = ambient + Lo + emissive;
    // 输出 HDR 线性色（由后处理 ToneMap 处理）
    FragColor = vec4(color, alpha);
}