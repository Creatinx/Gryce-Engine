#version 450 core

// 体积光柱片段着色器（Vulkan）
layout(location = 0) in vec3 vWorld;
layout(location = 1) in vec3 vAxis;
layout(location = 2) in vec3 vCam;
layout(location = 3) in vec4 vParams;
layout(location = 4) in vec4 vColor;

layout(location = 0) out vec4 FragColor;

void main() {
    vec3 to_frag = vWorld - vCam;
    float d = length(to_frag);
    vec3 dir = to_frag / max(d, 1e-4);

    float c = abs(dot(dir, normalize(vAxis)));
    float angular = smoothstep(vParams.z, vParams.y, c);
    float axial = 1.0 - vParams.x;
    float dist_fade = 1.0 - smoothstep(vParams.w, vParams.w * 2.5, d);

    float a = clamp(angular * axial * dist_fade, 0.0, 0.6) * vColor.a;
    if (a <= 0.002) discard;
    FragColor = vec4(vColor.rgb, a);
}