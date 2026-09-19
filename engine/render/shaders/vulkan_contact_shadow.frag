#version 450 core

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

// 场景深度（主 pass 写入，Depth24；非比较 sampler 读原始深度）
layout(binding = 0) uniform sampler2D uDepthTexture;

// 与 C++ VulkanShader::ContactShadowPushData 严格对齐（std430，48 字节）
layout(push_constant) uniform PushConstants {
    int cs_enabled;
    float cs_near;
    float cs_far;
    float cs_tan_half;
    float cs_aspect;
    float cs_radius;
    int cs_steps;
    float cs_strength;
    vec4 cs_light_dir_view;
} pc;

float linearize_depth(float d) {
    // 深度纹理存的是视图变换后的 [0,1] 深度，直接反算线性深度（勿再 *0.5+0.5）
    float d01 = d;
    return (2.0 * pc.cs_near * pc.cs_far) /
           (pc.cs_far + pc.cs_near - d01 * (pc.cs_far - pc.cs_near));
}

vec3 reconstruct_view_pos(vec2 uv, float lin) {
    vec2 ndc = uv * 2.0 - 1.0;
    return vec3(ndc.x * pc.cs_tan_half * pc.cs_aspect * lin,
                ndc.y * pc.cs_tan_half * lin,
                -lin);
}

void main() {
    if (pc.cs_enabled == 0) { FragColor = vec4(1.0); return; }
    float lin = linearize_depth(texture(uDepthTexture, vTexCoord).r);
    if (lin < 0.01 || lin >= pc.cs_far * 0.99) { FragColor = vec4(1.0); return; }

    vec3 P = reconstruct_view_pos(vTexCoord, lin);
    vec3 L = normalize(pc.cs_light_dir_view.xyz);
    float step_w = pc.cs_radius / float(max(pc.cs_steps, 1));
    float occ = 0.0;
    for (int s = 1; s <= pc.cs_steps; ++s) {
        vec3 Ps = P + L * (step_w * float(s));
        // 视图点投影回屏幕 uv
        vec2 proj = Ps.xy / (-Ps.z * pc.cs_tan_half);
        vec2 suv = proj * vec2(0.5 / pc.cs_aspect, 0.5) + 0.5;
        if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) continue;

        float lin2 = linearize_depth(texture(uDepthTexture, suv).r);
        if (lin2 < 0.01 || lin2 >= pc.cs_far * 0.99) continue;
        vec3 Q = reconstruct_view_pos(suv, lin2);

        // 视图空间 z 为负：Q 比 Ps 更靠近相机（Q.z > Ps.z）说明该方向有几何 → 接触遮挡
        if (Q.z > Ps.z + 1e-4) {
            occ += 1.0;
        }
    }
    float cs = 1.0 - (occ / float(max(pc.cs_steps, 1))) * pc.cs_strength;
    FragColor = vec4(vec3(clamp(cs, 0.0, 1.0)), 1.0);
}
