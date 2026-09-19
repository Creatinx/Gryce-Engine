#version 450 core

// 半透明水面材质片元：反射/折射混合 + 菲涅尔 + 泡沫。

layout(location = 0) in vec2 vTexCoord;
layout(location = 1) in vec3 vWorldPos;
layout(location = 2) in vec3 vNormal;
layout(location = 0) out vec4 FragColor;

// water 使用直接 slot<->binding（slot 0/1/2 = binding 0/1/2）：
//   0 = uReflectionTex, 1 = uRefractionTex, 2 = uDepthTex
layout(binding = 0) uniform sampler2D uReflectionTex;
layout(binding = 1) uniform sampler2D uRefractionTex;
layout(binding = 2) uniform sampler2D uDepthTex;

layout(push_constant) uniform PushConstants {
    mat4 view_proj;      // +0
    mat4 model;          // +64
    vec3 camera_pos;     // +128
    float water_height;  // +140
    float foam_amount;   // +144
    float time;          // +148
    float wave_amplitude;// +152
    float wave_frequency;// +156
    float wave_speed;    // +160
    float wave_steepness;// +164
    vec4 water_color;    // +176
} pc;

void main() {
    vec3 V = normalize(pc.camera_pos - vWorldPos);
    vec3 N = normalize(vNormal);
    vec3 R = reflect(-V, N);

    vec4 clip_pos = pc.view_proj * vec4(vWorldPos, 1.0);
    vec2 uv = clip_pos.xy / clip_pos.w * 0.5 + 0.5;

    float disturb = sin(vWorldPos.x * 0.5 + vWorldPos.z * 0.3 + pc.time * 0.8) * 0.01;
    vec2 reflect_uv = uv + vec2(disturb, disturb * 0.7);
    reflect_uv += R.xz * 0.05;

    vec3 reflection = texture(uReflectionTex, reflect_uv).rgb;
    vec3 refraction = texture(uRefractionTex, uv).rgb;

    float fresnel = pow(1.0 - max(dot(N, V), 0.0), 4.0);
    fresnel = mix(0.1, 1.0, fresnel);

    float foam = 0.0;
    float wave = sin(vWorldPos.x * pc.wave_frequency * 0.8 + pc.time * pc.wave_speed * 1.5) * 0.5 + 0.5;
    wave += sin(vWorldPos.z * pc.wave_frequency * 0.6 + pc.time * pc.wave_speed * 1.1) * 0.5 + 0.5;
    wave *= 0.5;
    foam = smoothstep(pc.foam_amount * 0.8, pc.foam_amount, wave);
    foam *= pc.foam_amount;

    vec3 color = mix(refraction, reflection, fresnel);
    color = mix(color, pc.water_color.rgb, 0.3);
    color += foam * vec3(1.0, 0.95, 0.9);

    float alpha = mix(0.6, 0.9, fresnel);
    FragColor = vec4(color, alpha);
}