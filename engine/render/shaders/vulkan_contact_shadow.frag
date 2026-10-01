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
    // 深度纹理存的是 w = far*(z-near)/((far-near)*z)（Vulkan 投影的 z 行映射到 [0,1]）
    return (pc.cs_near * pc.cs_far) /
           max(pc.cs_far - d * (pc.cs_far - pc.cs_near), 1e-6);
}

vec3 reconstruct_view_pos(vec2 uv, float lin) {
    // uv ↔ NDC：Vulkan 后处理 pass 的 vUV 是"v=0 在画面顶端"，与 OpenGL 相反，
    // 这里必须翻转 y。否则重建出的视图坐标上下镜像，遮挡判定全错
    //（表现：整片地面被压暗 0.4，而 GL 完全不受影响）。
    vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
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
    // 注意：这里的阈值故意保持很小（1e-4）。曾尝试放大到 step_w*0.5 去消除
    // 平坦表面上的"自遮挡"，但那会让判定正好落在深度量化噪声的量级上：
    // GL 与 Vulkan 的深度值有极小的差异，判定的"有/无遮挡"就会逐像素翻转，
    // 实测两端差异从 0.02% 上升到 49%（1e-2 级别的亮度差）。
    // 真正稳健的做法是改成"沿光源射线的距离比较"而不是视图深度差，留待后续。
    const float bias = 1e-4;
    float occ = 0.0;
    for (int s = 1; s <= pc.cs_steps; ++s) {
        vec3 Ps = P + L * (step_w * float(s));
        // 视图点投影回屏幕 uv（同样要把 y 反向映射回 VK 的 vUV 约定）
        vec2 proj = Ps.xy / (-Ps.z * pc.cs_tan_half);
        vec2 suv = vec2(proj.x * 0.5 / pc.cs_aspect + 0.5, 0.5 - proj.y * 0.5);
        if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) continue;

        float lin2 = linearize_depth(texture(uDepthTexture, suv).r);
        if (lin2 < 0.01 || lin2 >= pc.cs_far * 0.99) continue;
        vec3 Q = reconstruct_view_pos(suv, lin2);

        // 视图空间 z 为负：Q 比 Ps 更靠近相机（Q.z > Ps.z）说明该方向有几何 → 接触遮挡
        if (Q.z > Ps.z + bias) {
            occ += 1.0;
        }
    }
    float cs = 1.0 - (occ / float(max(pc.cs_steps, 1))) * pc.cs_strength;
    FragColor = vec4(vec3(clamp(cs, 0.0, 1.0)), 1.0);
}
