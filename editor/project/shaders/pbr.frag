#version 330 core

in vec3 vFragPos;
in vec2 vTexCoord;
in vec3 vColor;
in mat3 vTBN;
in vec4 vLightSpacePos;

out vec4 FragColor;

// ---------------------------------------------------------------------------
// 材质
// ---------------------------------------------------------------------------
uniform vec3 uAlbedoColor;
uniform float uRoughness;
uniform float uMetallic;
uniform float uAO;
uniform vec3 uEmissiveColor;
uniform float uOpacity;
uniform vec4 uUVTransform; // xy=scale, zw=offset

uniform sampler2D uAlbedoMap;
uniform sampler2D uNormalMap;
uniform sampler2D uRoughnessMap;
uniform sampler2D uMetallicMap;
uniform sampler2D uAOMap;
uniform sampler2D uEmissiveMap;
uniform sampler2DShadow uShadowMap;

uniform samplerCube uIrradianceMap;
uniform samplerCube uPrefilterMap;
uniform sampler2D uBRDFLUT;

uniform int uUseAlbedoMap;
uniform int uUseNormalMap;
uniform int uUseRoughnessMap;
uniform int uUseMetallicMap;
uniform int uUseAOMap;
uniform int uUseEmissiveMap;
uniform int uUseIBL;
uniform float uIBLIntensity;
uniform int uTwoSided; // 双面材质：背面翻转法线

// ---------------------------------------------------------------------------
// 灯光 / 相机
// ---------------------------------------------------------------------------
#define MAX_LIGHTS 8

uniform vec3 uCameraPos;
uniform vec3 uAmbient;
uniform int uLightCount;
uniform int uLightType[MAX_LIGHTS];   // 0=方向光 1=点光 2=聚光
uniform vec3 uLightPos[MAX_LIGHTS];
uniform vec3 uLightDir[MAX_LIGHTS];
uniform vec3 uLightColor[MAX_LIGHTS];
uniform float uLightIntensity[MAX_LIGHTS];
uniform vec4 uLightParams[MAX_LIGHTS]; // x=range, y=cos(outer), z=cos(inner)

uniform float uShadowBias;
uniform int uUseShadowMap;
uniform int uShadowLightIndex; // 产生阴影的方向光下标（-1 表示无）
uniform int uHDREnabled;       // 1=输出线性 HDR（由 tonemap pass 处理），0=内置 tonemap

const float PI = 3.14159265359;

// ---------------------------------------------------------------------------
// PBR 辅助函数
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Shadow
// ---------------------------------------------------------------------------
// 16-tap Poisson disk 软阴影采样：单 tap 硬件 PCF 在大阴影盒下会呈现
// 明显的 texel 网格；多 tap 按 texel 尺寸抖动后阴影过渡平滑。
const vec2 k_shadow_poisson[16] = vec2[](
    vec2(-0.94201624, -0.39906216), vec2( 0.94558609, -0.76890725),
    vec2(-0.09418410, -0.92938870), vec2( 0.34495938,  0.29387760),
    vec2(-0.91588581,  0.45771432), vec2(-0.81544232, -0.87912464),
    vec2(-0.38277543,  0.27676845), vec2( 0.97484398,  0.75648379),
    vec2( 0.44323325, -0.97511554), vec2( 0.53742981, -0.47373420),
    vec2(-0.26496911, -0.41893023), vec2( 0.79197514,  0.19090188),
    vec2(-0.24188840,  0.99706507), vec2(-0.81409955,  0.91437590),
    vec2( 0.19984126,  0.78641367), vec2( 0.14383161, -0.14100790));

// 廉价逐像素噪声（无需纹理）：用于旋转 Poisson 盘，把固定采样盘留下的
// texel 网格图案打散成细噪点，视觉上读作平滑过渡。
float interleaved_gradient_noise(vec2 pixel) {
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

float shadow_sample_pcf(vec3 coords) {
    vec2 texel = 1.0 / vec2(textureSize(uShadowMap, 0));
    const float radius = 2.0; // 采样半径（texel 单位）
    // 每像素随机旋转 Poisson 盘
    float angle = interleaved_gradient_noise(gl_FragCoord.xy) * 6.2831853;
    float s = sin(angle);
    float c = cos(angle);
    mat2 rot = mat2(c, -s, s, c);
    float lit = 0.0;
    for (int i = 0; i < 16; ++i) {
        vec2 offset = rot * k_shadow_poisson[i];
        lit += texture(uShadowMap, vec3(coords.xy + offset * texel * radius, coords.z));
    }
    return lit / 16.0;
}

float shadow_calculation(vec4 light_space_pos, vec3 normal, vec3 light_dir) {
    if (uUseShadowMap == 0) return 1.0;

    vec3 proj_coords = light_space_pos.xyz / light_space_pos.w;
    proj_coords = proj_coords * 0.5 + 0.5;

    if (proj_coords.z > 1.0) return 1.0;

    float current_depth = proj_coords.z;
    float bias = max(uShadowBias * (1.0 - dot(normal, light_dir)), uShadowBias * 0.1);

    // Poisson 软阴影采样
    vec3 coords = vec3(proj_coords.xy, current_depth - bias);
    float lit = shadow_sample_pcf(coords);

    // 阴影贴图覆盖范围边缘淡出为全亮：消除阴影盒边界的硬线（无"临界位置"）
    float edge = min(min(proj_coords.x, 1.0 - proj_coords.x),
                     min(proj_coords.y, 1.0 - proj_coords.y));
    float fade = smoothstep(0.0, 0.05, edge);
    return mix(1.0, lit, fade);
}

// ---------------------------------------------------------------------------
// 光照主函数
// ---------------------------------------------------------------------------
void main() {
    vec2 uv = vTexCoord * uUVTransform.xy + uUVTransform.zw;

    vec4 albedo_tex = texture(uAlbedoMap, uv);
    vec3 albedo = uUseAlbedoMap > 0 ? albedo_tex.rgb : uAlbedoColor;
    albedo *= vColor;
    float alpha = (uUseAlbedoMap > 0 ? albedo_tex.a : 1.0) * uOpacity;

    vec3 normal = uUseNormalMap > 0
        ? normalize(texture(uNormalMap, uv).rgb * 2.0 - 1.0)
        : vec3(0.0, 0.0, 1.0);
    vec3 N = normalize(vTBN * normal);
    // 双面材质：背面使用翻转法线，否则背面光照全错（发暗/发黑）
    if (uTwoSided != 0 && !gl_FrontFacing) N = -N;

    float roughness = uUseRoughnessMap > 0 ? texture(uRoughnessMap, uv).r : uRoughness;
    float metallic = uUseMetallicMap > 0 ? texture(uMetallicMap, uv).r : uMetallic;
    float ao = uUseAOMap > 0 ? texture(uAOMap, uv).r : uAO;

    vec3 V = normalize(uCameraPos - vFragPos);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    vec3 Lo = vec3(0.0);
    for (int i = 0; i < uLightCount; ++i) {
        vec3 L;
        vec3 radiance;
        float shadow = 1.0;

        if (uLightType[i] == 0) {
            // 方向光
            L = normalize(-uLightDir[i]);
            radiance = uLightColor[i] * uLightIntensity[i];
            if (i == uShadowLightIndex) {
                shadow = shadow_calculation(vLightSpacePos, N, L);
            }
        } else {
            // 点光 / 聚光
            vec3 to_light = uLightPos[i] - vFragPos;
            float dist = length(to_light);
            float range = uLightParams[i].x;
            if (dist > range) continue;
            L = to_light / dist;
            float attenuation = 1.0 - dist / range;
            attenuation *= attenuation;
            radiance = uLightColor[i] * uLightIntensity[i] * attenuation;

            if (uLightType[i] == 2) {
                // 聚光锥衰减
                float cos_angle = dot(-L, normalize(uLightDir[i]));
                float cos_outer = uLightParams[i].y;
                float cos_inner = uLightParams[i].z;
                float spot = smoothstep(cos_outer, cos_inner, cos_angle);
                if (spot <= 0.0) continue;
                radiance *= spot;
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

    vec3 ambient = uAmbient * albedo * ao;
    if (uUseIBL > 0) {
        vec3 Nsafe = N;
        vec3 irradiance = texture(uIrradianceMap, Nsafe).rgb;
        vec3 diffuse = irradiance * albedo;

        vec3 R = reflect(-V, Nsafe);
        vec3 prefiltered = texture(uPrefilterMap, R).rgb;
        vec2 brdf = texture(uBRDFLUT, vec2(max(dot(Nsafe, V), 0.0), roughness)).rg;
        vec3 F_ibl = fresnel_schlick(max(dot(Nsafe, V), 0.0), F0);
        vec3 specular = prefiltered * (F_ibl * brdf.x + brdf.y);

        vec3 kD = (vec3(1.0) - F_ibl) * (1.0 - metallic);
        ambient = (kD * diffuse + specular) * ao * uIBLIntensity;
    }
    vec3 emissive = uEmissiveColor * (uUseEmissiveMap > 0 ? texture(uEmissiveMap, uv).rgb : vec3(1.0));

    vec3 color = ambient + Lo + emissive;

    if (uHDREnabled == 0) {
        // LDR 路径：内置 Reinhard + gamma（HDR 开启时由 tonemap pass 统一处理）
        color = color / (color + vec3(1.0));
        color = pow(color, vec3(1.0 / 2.2));
    }

    FragColor = vec4(color, alpha);
}
