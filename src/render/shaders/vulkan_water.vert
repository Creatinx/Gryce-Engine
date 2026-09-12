#version 450 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;

layout(location = 0) out vec2 vTexCoord;
layout(location = 1) out vec3 vWorldPos;
layout(location = 2) out vec3 vNormal;

// 对应 C++ VulkanShader::WaterPushData（192 字节）。顶点阶段读 view_proj/model 及波浪参数。
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
    vec4 world_pos = pc.model * vec4(aPos, 1.0);
    vec3 pos = world_pos.xyz;

    float freq1 = pc.wave_frequency;
    float freq2 = pc.wave_frequency * 1.3;
    float freq3 = pc.wave_frequency * 0.5;

    float speed1 = pc.time * pc.wave_speed;
    float speed2 = pc.time * pc.wave_speed * 0.7;
    float speed3 = pc.time * pc.wave_speed * 1.2;

    float w1 = pos.x * freq1 + speed1;
    float w2 = pos.z * freq2 + speed2;
    float w3 = (pos.x + pos.z) * freq3 + speed3;

    float wave1 = sin(w1) * pc.wave_amplitude;
    float wave2 = sin(w2) * pc.wave_amplitude * 0.6;
    float wave3 = sin(w3) * pc.wave_amplitude * 0.3;
    pos.y += wave1 + wave2 + wave3;

    float steep = pc.wave_steepness * pc.wave_amplitude;
    pos.x += cos(w1) * steep * 0.3;
    pos.z += cos(w2) * steep * 0.3;

    float dw1_dx = cos(w1) * freq1 * pc.wave_amplitude;
    float dw2_dz = cos(w2) * freq2 * pc.wave_amplitude * 0.6;
    float dw3_dx = cos(w3) * freq3 * pc.wave_amplitude * 0.3;
    float dw3_dz = dw3_dx;
    vec3 normal = normalize(vec3(-(dw1_dx + dw3_dx), 1.0, -(dw2_dz + dw3_dz)));

    vTexCoord = aTexCoord * 10.0;
    vWorldPos = pos;
    vNormal = normal;

    gl_Position = pc.view_proj * vec4(pos, 1.0);
}