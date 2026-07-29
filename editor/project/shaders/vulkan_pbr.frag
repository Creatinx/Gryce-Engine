#version 450 core

layout(location = 0) in vec3 vFragPos;
layout(location = 1) in vec2 vTexCoord;
layout(location = 2) in vec3 vColor;
layout(location = 3) in mat3 vTBN;
layout(location = 6) in vec4 vLightSpacePos;

layout(location = 0) out vec4 FragColor;

#define MAX_LIGHTS 8

struct Light {
    vec4 pos_type;        // xyz=position, w=type (0 方向光/1 点光/2 聚光)
    vec4 dir_range;       // xyz=direction, w=range
    vec4 color_intensity; // xyz=color, w=intensity
    vec4 spot;            // x=cos(outer), y=cos(inner)
};

// 与 C++ VulkanShader::UBOData 一一对应（std140）
layout(set = 0, binding = 0) uniform MaterialLightUBO {
    vec4 uAlbedoColor;
    vec4 uCameraPos;
    vec4 uEmissiveOpacity; // xyz=emissive, w=opacity
    vec4 uAmbient;         // xyz=环境光颜色
    vec4 uUVTransform;     // xy=scale, zw=offset
    float uRoughness;
    float uMetallic;
    float uAO;
    float uShadowBias;
    int uUseShadowMap;
    int uUseAlbedoMap;
    int uUseNormalMap;
    int uUseRoughnessMap;
    int uUseMetallicMap;
    int uUseAOMap;
    int uUseEmissiveMap;
    int uHDREnabled;       // 1=输出线性 HDR，0=内置 tonemap
    int uLightCount;
    int uShadowLightIndex; // 产生阴影的方向光下标（-1 表示无）
    int uUseIBL;
    float uIBLIntensity;
    int uTwoSided;         // 双面材质：背面翻转法线
    Light uLights[MAX_LIGHTS];
} ubo;

layout(set = 0, binding = 1) uniform sampler2D uAlbedoMap;
layout(set = 0, binding = 2) uniform sampler2D uNormalMap;
layout(set = 0, binding = 3) uniform sampler2D uRoughnessMap;
layout(set = 0, binding = 4) uniform sampler2D uMetallicMap;
layout(set = 0, binding = 5) uniform sampler2D uAOMap;
layout(set = 0, binding = 6) uniform sampler2DShadow uShadowMap;
layout(set = 0, binding = 7) uniform sampler2D uEmissiveMap;

layout(set = 0, binding = 9) uniform samplerCube uIrradianceMap;
layout(set = 0, binding = 10) uniform samplerCube uPrefilterMap;
layout(set = 0, binding = 11) uniform sampler2D uBRDFLUT;

const float PI = 3.14159265359;

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
    if (ubo.uUseShadowMap == 0) return 1.0;

    vec3 proj_coords = light_space_pos.xyz / light_space_pos.w;
    // Vulkan NDC z 已是 [0,1]，只有 xy 需要从 [-1,1] 重映射
    proj_coords.xy = proj_coords.xy * 0.5 + 0.5;
    // Vulkan 使用负 viewport 高度模拟 OpenGL 坐标系，
    // 渲染到 shadow map 时 clip y=1 会落到图像顶部（v=0），
    // 所以采样前需要把 y 翻转。
    proj_coords.y = 1.0 - proj_coords.y;

    if (proj_coords.z > 1.0) return 1.0;

    float current_depth = proj_coords.z;
    // 参考 Godot：法线方向动态 bias，避免 NdotL 为负时 bias 过大
    float ndotl = clamp(dot(normal, light_dir), 0.0, 1.0);
    float bias = max(ubo.uShadowBias * (1.0 - ndotl), ubo.uShadowBias * 0.1);

    vec3 coords = vec3(proj_coords.xy, current_depth - bias);
    float lit = shadow_sample_pcf(coords);

    // 阴影贴图覆盖范围边缘淡出为全亮：消除阴影盒边界的硬线（无"临界位置"）
    float edge = min(min(proj_coords.x, 1.0 - proj_coords.x),
                     min(proj_coords.y, 1.0 - proj_coords.y));
    float fade = smoothstep(0.0, 0.05, edge);
    return mix(1.0, lit, fade);
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
    // 双面材质：背面使用翻转法线，否则背面光照全错（发暗/发黑）
    if (ubo.uTwoSided != 0 && !gl_FrontFacing) N = -N;

    float roughness = ubo.uUseRoughnessMap > 0 ? texture(uRoughnessMap, uv).r : ubo.uRoughness;
    float metallic = ubo.uUseMetallicMap > 0 ? texture(uMetallicMap, uv).r : ubo.uMetallic;
    float ao = ubo.uUseAOMap > 0 ? texture(uAOMap, uv).r : ubo.uAO;

    vec3 V = normalize(ubo.uCameraPos.xyz - vFragPos);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    vec3 Lo = vec3(0.0);
    for (int i = 0; i < ubo.uLightCount; ++i) {
        Light light = ubo.uLights[i];
        int light_type = int(light.pos_type.w + 0.5);
        vec3 L;
        vec3 radiance;
        float shadow = 1.0;

        if (light_type == 0) {
            // 方向光
            L = normalize(-light.dir_range.xyz);
            radiance = light.color_intensity.xyz * light.color_intensity.w;
            if (i == ubo.uShadowLightIndex) {
                shadow = shadow_calculation(vLightSpacePos, N, L);
            }
        } else {
            // 点光 / 聚光
            vec3 to_light = light.pos_type.xyz - vFragPos;
            float dist = length(to_light);
            float range = light.dir_range.w;
            if (dist > range) continue;
            L = to_light / dist;
            // 参考 Godot get_omni_attenuation：平滑二次衰减
            float nd = dist / range;
            nd *= nd;
            nd *= nd; // nd^4
            nd = max(1.0 - nd, 0.0);
            nd *= nd; // nd^2
            float attenuation = nd;
            radiance = light.color_intensity.xyz * light.color_intensity.w * attenuation;

            if (light_type == 2) {
                float cos_angle = dot(-L, normalize(light.dir_range.xyz));
                float spot = smoothstep(light.spot.x, light.spot.y, cos_angle);
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

    vec3 ambient = ubo.uAmbient.rgb * albedo * ao;
    if (ubo.uUseIBL > 0) {
        vec3 Nsafe = N;
        vec3 irradiance = texture(uIrradianceMap, Nsafe).rgb;
        vec3 diffuse = irradiance * albedo;

        vec3 R = reflect(-V, Nsafe);
        vec3 prefiltered = texture(uPrefilterMap, R).rgb;
        vec2 brdf = texture(uBRDFLUT, vec2(max(dot(Nsafe, V), 0.0), roughness)).rg;
        vec3 F_ibl = fresnel_schlick(max(dot(Nsafe, V), 0.0), F0);
        vec3 specular = prefiltered * (F_ibl * brdf.x + brdf.y);

        vec3 kD = (vec3(1.0) - F_ibl) * (1.0 - metallic);
        ambient = (kD * diffuse + specular) * ao * ubo.uIBLIntensity;
    }
    vec3 emissive = ubo.uEmissiveOpacity.xyz * (ubo.uUseEmissiveMap > 0 ? texture(uEmissiveMap, uv).rgb : vec3(1.0));

    vec3 color = ambient + Lo + emissive;

    if (ubo.uHDREnabled == 0) {
        // LDR 路径：内置 Reinhard + gamma（HDR 开启时由 tonemap pass 统一处理）
        color = color / (color + vec3(1.0));
        color = pow(color, vec3(1.0 / 2.2));
    }

    FragColor = vec4(color, alpha);
}
