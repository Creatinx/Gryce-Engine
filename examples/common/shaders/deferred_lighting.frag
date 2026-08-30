#version 330 core

// [Shader 阶段] Fragment Shader
// [功能] 延迟光照 Pass：从 GBuffer 采样并计算光照
// [输入] GBuffer 纹理（albedo+metallic, normal+roughness, emissive+AO, depth），
//        光源参数，CSM 阴影贴图
// [输出] HDR 颜色

in vec2 vUV;

out vec4 FragColor;

// ---- GBuffer 采样 ----
uniform sampler2D uGBufferAlbedo;    // RGB=albedo, A=metallic
uniform sampler2D uGBufferNormal;    // RGB=normal(enc), A=roughness
uniform sampler2D uGBufferEmissive;  // RGB=emissive, A=AO
uniform sampler2D uGBufferDepth;     // R=depth

// ---- 相机 ----
uniform vec3 uCameraPos;
uniform vec3 uAmbient;
uniform mat4 uViewMatrix;
uniform mat4 uProjectionMatrix;
uniform mat4 uInvViewProj;
uniform float uNear;
uniform float uFar;

// ---- 光源（最多 8 个） ----
#define MAX_LIGHTS 8
uniform int uLightCount;
uniform int uLightType[MAX_LIGHTS];
uniform vec3 uLightPos[MAX_LIGHTS];
uniform vec3 uLightDir[MAX_LIGHTS];
uniform vec3 uLightColor[MAX_LIGHTS];
uniform float uLightIntensity[MAX_LIGHTS];
uniform vec4 uLightParams[MAX_LIGHTS];

// ---- CSM 级联阴影 ----
uniform int uUseShadowMap;
uniform int uShadowLightIndex;
uniform sampler2DShadow uShadowMap;
uniform sampler2DShadow uShadowMap1;
uniform sampler2DShadow uShadowMap2;
uniform sampler2DShadow uShadowMap3;
uniform sampler2D uShadowMapDepth;
uniform sampler2D uShadowMapDepth1;
uniform sampler2D uShadowMapDepth2;
uniform sampler2D uShadowMapDepth3;

// ---- VSM/ESM 阴影纹理（RGBA16F 颜色纹理，与 PCF 共享级联） ----
uniform sampler2D uVSMTexture0;
uniform sampler2D uVSMTexture1;
uniform sampler2D uVSMTexture2;
uniform sampler2D uVSMTexture3;

uniform int uShadowMode;           // 0=PCF, 1=VSM, 2=ESM
uniform float uESMExponent;        // ESM 指数参数（默认 40.0）

uniform int uCascadeCount;
uniform vec4 uCascadeSplits;
uniform vec4 uCascadeFarBlend;
uniform vec4 uCascadeBias;
uniform mat4 uCascadeLightSpace[4];
uniform int uPCSSEnabled;
uniform float uPCSSLightSize;
uniform float uPCSSMaxRadius;
uniform float uPCSSBlockerScale;

// ---- IBL ----
uniform samplerCube uIrradianceMap;
uniform samplerCube uPrefilterMap;
uniform sampler2D uBRDFLUT;
uniform int uUseIBL;
uniform float uIBLIntensity;

// ---- SSAO ----
uniform sampler2D uSSAOTexture;
uniform int uUseSSAO;
uniform float uSSAOStrength;

// ---- GI 全局光照（SDFGI / VoxelGI） ----
uniform sampler2D uGITexture;
uniform int uGIEnabled;
uniform int uGIMode; // 0=None, 1=SDFGI, 2=VoxelGI
uniform float uGIIndirectIntensity;

// ---- 点光源阴影（双抛物面映射） ----
uniform int uPointShadowCount;
uniform sampler2D uPointShadowMap0;
uniform sampler2D uPointShadowMap1;
uniform vec3 uPointLightPosWorld[4];
uniform float uPointLightRange[4];

const float PI = 3.14159265359;

// ===========================================================================
// 点光源双抛物面阴影 + PCF（与 pbr.frag 一致）
// ===========================================================================
float point_shadow_sample(vec3 frag_pos, vec3 light_pos, float range, sampler2D shadow_tex) {
    vec3 to_light = frag_pos - light_pos;
    float dist = length(to_light);
    if (dist >= range) return 1.0;
    vec3 dir = to_light / dist;
    float a = 1.0 / (1.0 + abs(dir.z));
    vec2 uv = dir.xy * a * 0.5 + 0.5;
    float depth = dist / range;
    float angle = interleaved_gradient_noise(gl_FragCoord.xy) * 6.2831853;
    float s = sin(angle);
    float c = cos(angle);
    mat2 rot = mat2(c, -s, s, c);
    float texel = 1.0 / float(textureSize(shadow_tex, 0));
    float lit = 0.0;
    for (int i = 0; i < 16; ++i) {
        vec2 offset = rot * k_shadow_poisson[i] * texel * 2.0;
        float d = texture(shadow_tex, uv + offset).r;
        lit += (depth - 0.005 < d) ? 1.0 : 0.0;
    }
    return lit / 16.0;
}

float point_shadow(vec3 frag_pos, int light_index, vec3 light_pos, float range) {
    if (uPointShadowCount <= 0 || light_index >= uPointShadowCount) return 1.0;
    if (light_index == 0) return point_shadow_sample(frag_pos, light_pos, range, uPointShadowMap0);
    if (light_index == 1) return point_shadow_sample(frag_pos, light_pos, range, uPointShadowMap1);
    return 1.0;
}

// ===========================================================================
// 世界空间重建
// ===========================================================================
vec3 world_from_depth(float depth, vec2 uv) {
    vec4 clip = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 world = uInvViewProj * clip;
    return world.xyz / world.w;
}

// ===========================================================================
// PBR
// ===========================================================================
float distribution_ggx(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    return a2 / (PI * denom * denom + 0.0001);
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
// CSM 阴影（与 scene_pbr.frag 一致）
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
    float penumbra = uPCSSLightSize * (receiver - avg_blocker) / max(avg_blocker, 1e-4);
    penumbra = clamp(penumbra * uPCSSBlockerScale, 1.0, uPCSSMaxRadius);
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
    float edge = min(min(proj.x, 1.0 - proj.x), min(proj.y, 1.0 - proj.y));
    float fade = smoothstep(0.0, 0.05, edge);
    lit = mix(1.0, lit, fade);
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
// VSM 采样: Chebyshev 不等式求最大概率
// ===========================================================================
float vsm_sample(sampler2D vsm_tex, vec2 uv, float frag_depth) {
    vec2 moments = texture(vsm_tex, uv).rg;
    if (frag_depth <= moments.x) return 1.0;
    float variance = max(moments.y - moments.x * moments.x, 0.00001);
    float d = frag_depth - moments.x;
    float p_max = variance / (variance + d * d);
    return p_max;
}

// ===========================================================================
// ESM 采样
// ===========================================================================
float esm_sample(sampler2D esm_tex, vec2 uv, float frag_depth, float exponent) {
    float occluded = texture(esm_tex, uv).r;
    return clamp(exp(exponent * (occluded + exp(-exponent * frag_depth))) - 1.0, 0.0, 1.0);
}

// 工具：根据级联索引返回 VSM/ESM 纹理
sampler2D vsm_tex_for_cascade(int cascade) {
    if (cascade == 0) return uVSMTexture0;
    if (cascade == 1) return uVSMTexture1;
    if (cascade == 2) return uVSMTexture2;
    return uVSMTexture3;
}

// ===========================================================================
// VSM/ESM 级联阴影采样（替代 PCF/PCSS 路径）
// ===========================================================================
float cascade_shadow_vsm_esm(vec3 frag_pos, vec3 normal, vec3 light_dir, out int out_cascade) {
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

    float lit = 1.0;
    vec2 sm_uv = proj.xy;
    float frag_depth = proj.z;
    sampler2D sm_tex = vsm_tex_for_cascade(cascade);

    if (uShadowMode == 1) {
        // VSM: Chebyshev 不等式
        lit = vsm_sample(sm_tex, sm_uv, frag_depth);
    } else {
        // ESM: 指数深度比较
        lit = esm_sample(sm_tex, sm_uv, frag_depth, uESMExponent);
    }

    // 阴影贴图覆盖范围边缘淡出为全亮，消除硬边
    float edge = min(min(proj.x, 1.0 - proj.x), min(proj.y, 1.0 - proj.y));
    float fade = smoothstep(0.0, 0.05, edge);
    lit = mix(1.0, lit, fade);

    // Cascade Blend：级联边界交叉淡入淡出
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
                sampler2D sm_tex2 = vsm_tex_for_cascade(c2);
                if (uShadowMode == 1) {
                    lit2 = vsm_sample(sm_tex2, p2.xy, p2.z);
                } else {
                    lit2 = esm_sample(sm_tex2, p2.xy, p2.z, uESMExponent);
                }
            }
            lit = mix(lit, lit2, t);
        }
    }
    return lit;
}

// ===========================================================================
// 主函数
// ===========================================================================
void main() {
    // 从 GBuffer 采样
    float depth = texture(uGBufferDepth, vUV).r;
    if (depth >= 1.0) { discard; return; }

    vec3 world_pos = world_from_depth(depth, vUV);
    vec4 albedo_metallic = texture(uGBufferAlbedo, vUV);
    vec3 albedo = albedo_metallic.rgb;
    float metallic = albedo_metallic.a;

    vec4 normal_roughness = texture(uGBufferNormal, vUV);
    vec3 N = normalize(normal_roughness.xyz * 2.0 - 1.0);
    float roughness = normal_roughness.a;

    vec4 emissive_ao = texture(uGBufferEmissive, vUV);
    vec3 emissive = emissive_ao.rgb;
    float ao = emissive_ao.a;

    vec3 V = normalize(uCameraPos - world_pos);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    vec3 Lo = vec3(0.0);
    float shadow_factor = 1.0;
    int shadow_cascade = 0;

    for (int i = 0; i < uLightCount; ++i) {
        vec3 L;
        vec3 radiance;
        float shadow = 1.0;

        if (uLightType[i] == 0) {
            // 方向光
            L = normalize(-uLightDir[i]);
            radiance = uLightColor[i] * uLightIntensity[i];
            if (i == uShadowLightIndex) {
                if (uShadowMode == 0) {
                    shadow = cascade_shadow(world_pos, N, L, shadow_cascade);
                } else {
                    shadow = cascade_shadow_vsm_esm(world_pos, N, L, shadow_cascade);
                }
                shadow_factor = shadow;
            }
        } else {
            // 点光 / 聚光
            vec3 to_light = uLightPos[i] - world_pos;
            float dist = length(to_light);
            float range = uLightParams[i].x;
            if (dist >= range) continue;
            L = to_light / dist;
            float atten = 1.0 / max(dist * dist, 1e-3);
            float cutoff = 1.0 - smoothstep(range * 0.7, range, dist);
            radiance = uLightColor[i] * uLightIntensity[i] * atten * cutoff;
            if (uLightType[i] == 2) {
                float cos_angle = dot(-L, normalize(uLightDir[i]));
                float cos_outer = uLightParams[i].y;
                float cos_inner = uLightParams[i].z;
                float spot = smoothstep(cos_outer, cos_inner, cos_angle);
                if (spot <= 0.0) continue;
                radiance *= spot;
            }

            // 点光源阴影（双抛物面 PCF）
            if (uLightType[i] == 1 && uPointShadowCount > 0) {
                shadow = point_shadow(world_pos, i, uLightPos[i], range);
            }
        }

        vec3 H = normalize(V + L);
        float NDF = distribution_ggx(N, H, roughness);
        float G = geometry_smith(N, V, L, roughness);
        vec3 F = fresnel_schlick(max(dot(H, V), 0.0), F0);

        vec3 kS = F;
        vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);

        vec3 numerator = NDF * G * F;
        float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.0001;
        vec3 specular = numerator / denominator;

        float NdotL = max(dot(N, L), 0.0);
        Lo += (kD * albedo / PI + specular) * radiance * NdotL * shadow;
    }

    // 环境光 + IBL
    vec3 ambient = uAmbient * albedo * ao;
    if (uUseSSAO != 0) {
        float ssao = texture(uSSAOTexture, vUV).r;
        ambient *= mix(1.0, ssao, uSSAOStrength);
    }
    if (uUseIBL > 0) {
        vec3 irradiance = texture(uIrradianceMap, N).rgb;
        vec3 diffuse = irradiance * albedo;
        vec3 R = reflect(-V, N);
        vec3 prefiltered = textureLod(uPrefilterMap, R, roughness * 4.0).rgb;
        vec2 brdf = texture(uBRDFLUT, vec2(max(dot(N, V), 0.0), roughness)).rg;
        vec3 F_ibl = fresnel_schlick(max(dot(N, V), 0.0), F0);
        vec3 specular = prefiltered * (F_ibl * brdf.x + brdf.y);
        vec3 kD = (vec3(1.0) - F_ibl) * (1.0 - metallic);
        ambient = (kD * diffuse + specular) * ao * uIBLIntensity;
    }

    // GI 全局光照间接采样（叠加到环境光之上）
    if (uGIEnabled != 0) {
        vec3 gi_indirect = texture(uGITexture, vUV).rgb;
        ambient += gi_indirect * albedo * uGIIndirectIntensity;
    }

    vec3 color = ambient + Lo + emissive;
    FragColor = vec4(color, 1.0);
}