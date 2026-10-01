#version 330 core

// 体积光柱片段着色器：角向 + 轴向 + 相机距离三重落点，得到柔和的光柱。
in vec3 vWorld;
in vec3 vAxis;
in vec3 vCam;
in vec4 vParams;
in vec4 vColor;

out vec4 FragColor;

void main() {
    vec3 to_frag = vWorld - vCam;
    float d = length(to_frag);
    vec3 dir = to_frag / max(d, 1e-4);

    // 角向落点：视线越接近平行光轴越亮（正对光柱看进去最亮）
    float c = abs(dot(dir, normalize(vAxis)));
    float angular = smoothstep(vParams.z, vParams.y, c);

    // 轴向落点：近端最强，远端淡出（1 - t）
    float axial = 1.0 - vParams.x;

    // 相机距离淡出：超过光柱长度后逐步消散
    float dist_fade = 1.0 - smoothstep(vParams.w, vParams.w * 2.5, d);

    float a = clamp(angular * axial * dist_fade, 0.0, 0.6) * vColor.a;
    if (a <= 0.002) discard;
    FragColor = vec4(vColor.rgb, a);
}