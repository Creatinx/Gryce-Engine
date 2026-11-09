#version 330 core

// 体积光散射（丁达尔 / 神光 / 光柱）
// 沿视线在参与介质中按光线步进，对每个采样点做方向光的单散射累积：
//   in_scatter += sunColor * tint * HG(phase) * sigma_s * transmittance * stepLen
// 并随视线推进累积消光（transmittance *= exp(-sigma_t*stepLen)）。
//
// "是否被太阳照亮"用**平行光阴影贴图**判定：把采样点变换到光照空间，与阴影
// 深度比对。这是丁达尔的关键 —— 只有被几何挡住阳光的区域才会暗下去，于是
// 窗口 / 缝隙 / 树影之间留下可见的光柱；单纯按屏幕深度判空只会得到一层均匀薄雾。
// 采样点落在阴影图覆盖范围之外时按"被照亮"处理（远处不硬切，避免矩形边界）。
//
// 本 pass 输出 scene + in_scatter，调用方把它接成新的 HDR 颜色输入。

in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

uniform sampler2D uSceneTex;   // HDR 场景色（合成后含 SSR / 光追反射）
uniform sampler2D uDepthTex;   // 场景深度（[0,1]，取 .r）
uniform sampler2D uShadowMap;  // 平行光级联 0 的阴影深度（原始深度，非比较采样器）
uniform mat4 uInvViewProj;     // 世界 -> NDC 的逆（由深度重建世界坐标）
uniform mat4 uLightSpaceMatrix; // 世界 -> 光照裁剪空间（与阴影贴图同源矩阵）
uniform vec3 uCameraPos;
uniform vec3 uSunDirection;     // 指向太阳
uniform vec3 uSunColor;
uniform float uSunIntensity;
uniform vec3 uTint;            // 介质染色（彩色丁达尔）
uniform float uIntensity;      // 整体强度
uniform float uDensity;        // 介质密度（每世界单位消光尺度）
uniform float uScattering;     // 散射占比（相对消光）
uniform float uPhaseG;         // Henyey-Greenstein 各向异性
uniform int   uSteps;          // 视线步进采样数
uniform float uMaxDistance;    // 单条视线最大追踪距离
uniform int   uShadowEnabled;  // 0 = 不做日照遮挡（退化成均匀薄雾）
uniform float uShadowBias;     // 阴影深度比较偏移（[0,1] 归一化深度）
// GL 下 NDC z = depth*2-1；Vulkan/D3D12 下 NDC z = depth（[0,1]）。由管线按后端下发。
uniform float uNDCZScale;
uniform float uNDCZBias;

const int kMaxSteps = 64;

vec3 world_from_depth(float depth, vec2 uv) {
    float ndc_z = depth * uNDCZScale + uNDCZBias;
    vec4 ndc = vec4(uv * 2.0 - 1.0, ndc_z, 1.0);
    vec4 world = uInvViewProj * ndc;
    return world.xyz / world.w;
}

// Henyey-Greenstein 相位函数
float hg_phase(float cos_theta, float g) {
    float g2 = g * g;
    float denom = 1.0 + g2 - 2.0 * g * cos_theta;
    // 省掉 1/(4π) 归一因子：HG 的物理原值只有 1e-2 量级，后向散射区域乘完介质
    // 密度后几乎为 0，表现成"强度调到 1 也看不见"。去掉这个常数后前向峰值
    // ≈(1+g)/(1-g)、后向≈(1-g)/(1+g)，落在 0.1~10，强度参数才有意义。
    return (1.0 - g2) / pow(max(denom, 1e-4), 1.5);
}

// 采样点是否被阳光直射（阴影贴图判定）。
bool sunlit(vec3 p) {
    if (uShadowEnabled == 0) return true;
    vec4 lp = uLightSpaceMatrix * vec4(p, 1.0);
    if (abs(lp.w) < 1e-6) return true;
    vec3 pc = lp.xyz / lp.w;
    pc = pc * 0.5 + 0.5;
    if (pc.x < 0.0 || pc.x > 1.0 || pc.y < 0.0 || pc.y > 1.0 || pc.z > 1.0) return true;
    return pc.z - uShadowBias <= texture(uShadowMap, pc.xy).r;
}

void main() {
    if (uSteps <= 0 || uDensity <= 0.0) {
        FragColor = vec4(texture(uSceneTex, vTexCoord).rgb, 1.0);
        return;
    }

    float depth = texture(uDepthTex, vTexCoord).r;
    vec3 ro = uCameraPos;
    vec3 surface = world_from_depth(min(depth, 0.9999), vTexCoord);
    float dist = (depth >= 1.0) ? uMaxDistance : length(surface - ro);
    vec3 rd = (depth >= 1.0)
                  ? normalize(world_from_depth(1.0, vTexCoord) - ro)   // 天空：取远平面方向
                  : normalize(surface - ro);

    vec3 sun_dir = normalize(uSunDirection);
    float phase = hg_phase(dot(rd, sun_dir), uPhaseG);

    int steps = min(uSteps, kMaxSteps);
    float step_len = dist / float(steps);
    vec3 step_vec = rd * step_len;

    vec3 sun_col = uSunColor * uSunIntensity;
    float sigma_t = uDensity;
    float sigma_s = uDensity * uScattering;

    float transmittance = 1.0;
    vec3 scatter = vec3(0.0);

    vec3 P = ro + step_vec * 0.5;
    for (int i = 0; i < kMaxSteps; ++i) {
        if (i >= steps) break;

        if (sunlit(P)) {
            vec3 S = sun_col * uTint * phase * sigma_s;
            scatter += S * transmittance * step_len;
        }
        transmittance *= exp(-sigma_t * step_len);

        P += step_vec;
    }

    vec3 scene = texture(uSceneTex, vTexCoord).rgb;
    FragColor = vec4(scene + scatter * uIntensity, 1.0);
}
